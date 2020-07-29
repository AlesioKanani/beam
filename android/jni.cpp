// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "wallet/core/wallet.h"
#include "wallet/core/wallet_db.h"
#include "wallet/core/wallet_network.h"
#include "wallet/client/wallet_model_async.h"
#include "wallet/core/default_peers.h"
#include "keykeeper/local_private_key_keeper.h"
#include "wallet/transactions/lelantus/pull_transaction.h"
#include "wallet/transactions/lelantus/push_transaction.h"
#include "wallet/transactions/lelantus/unlink_transaction.h"
#include "wallet/core/simple_transaction.h"
#include "wallet/core/common_utils.h"

#include "utility/bridge.h"
#include "utility/string_helpers.h"
#include "mnemonic/mnemonic.h"

#include <boost/filesystem.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <jni.h>

#include "common.h"
#include "wallet_model.h"
#include "node_model.h"
#include "version.h"

#define WALLET_FILENAME "wallet.db"
#define BBS_FILENAME "keys.bbs"

using namespace beam;
using namespace beam::wallet;
using namespace beam::io;
using namespace std;

namespace fs = boost::filesystem;

namespace
{
    static const unsigned LOG_ROTATION_PERIOD = 3 * 60 * 60 * 1000; // 3 hours

    // this code for node
    static unique_ptr<NodeModel> nodeModel;

    static unique_ptr<WalletModel> walletModel;

    static IWalletDB::Ptr walletDB;
    static Reactor::Ptr reactor;

    static ECC::NoLeak<ECC::uintBig> passwordHash;

    void initLogger(const string& appData, const string& appVersion)
    {
        static auto logger = Logger::create(LOG_LEVEL_DEBUG, LOG_LEVEL_DEBUG, LOG_LEVEL_DEBUG, "wallet_", (fs::path(appData) / fs::path("logs")).string());

        Rules::get().pForks[1].m_Height = 10;
        Rules::get().pForks[2].m_Height = 20;
        Rules::get().MaxRollback = 10;
        Rules::get().CA.LockPeriod = 10;
        Rules::get().Shielded.m_ProofMax.n = 4;
        Rules::get().Shielded.m_ProofMax.M = 3;
        Rules::get().Shielded.m_ProofMin.n = 4;
        Rules::get().Shielded.m_ProofMin.M = 2;
        Rules::get().Shielded.MaxWindowBacklog = 150;
        Rules::get().UpdateChecksum();
        
        LOG_INFO() << "Beam Mobile Wallet " << appVersion << " (" << BRANCH_NAME << ") library: " << PROJECT_VERSION;
        LOG_INFO() << "Rules signature: " << Rules::get().get_SignatureStr();
    }

    std::map<Notification::Type,bool> initNotifications(bool initialValue)
    {
        return std::map<Notification::Type,bool> {
            { Notification::Type::SoftwareUpdateAvailable,   false },
            { Notification::Type::BeamNews,                  false },
            { Notification::Type::WalletImplUpdateAvailable, initialValue },
            { Notification::Type::TransactionCompleted,      initialValue },
            { Notification::Type::TransactionFailed,         initialValue },
            { Notification::Type::AddressStatusChanged,      false }
        };
    }

}


#ifdef __cplusplus
extern "C" {
#endif


 JNIEXPORT jboolean JNICALL BEAM_JAVA_WALLET_INTERFACE(isAddress)(JNIEnv *env, jobject thiz, jstring address)
 {
    LOG_DEBUG() << "isAddress()";
    
    return beam::wallet::CheckReceiverAddress(JString(env, address).value());
 }

 JNIEXPORT jboolean JNICALL BEAM_JAVA_WALLET_INTERFACE(isToken)(JNIEnv *env, jobject thiz, jstring token)
 {
    LOG_DEBUG() << "isToken()";

    auto params = beam::wallet::ParseParameters(JString(env, token).value());
    return params && params->GetParameter<beam::wallet::TxType>(beam::wallet::TxParameterID::TransactionType);
 }

 JNIEXPORT jobject JNICALL BEAM_JAVA_WALLET_INTERFACE(generateToken)(JNIEnv *env, jobject thiz)
 {
    
    LOG_DEBUG() << "generateToken()";

    auto address = walletModel->generateToken(walletDB);

    TxParameters params;
    params.SetParameter(TxParameterID::PeerID, address.m_walletID);
    params.SetParameter(TxParameterID::PeerWalletIdentity, address.m_Identity);
    params.SetParameter(TxParameterID::TransactionType, beam::wallet::TxType::Simple);

    auto token = to_string(params);

    jobject jAddress = env->AllocObject(WalletAddressClass);

    {
        setStringField(env, WalletAddressClass, jAddress, "walletID", to_string(address.m_walletID));
        setStringField(env, WalletAddressClass, jAddress, "label", address.m_label);
        setStringField(env, WalletAddressClass, jAddress, "category", address.m_category);
        setLongField(env, WalletAddressClass, jAddress, "createTime", address.m_createTime);
        setLongField(env, WalletAddressClass, jAddress, "duration", address.m_duration);
        setLongField(env, WalletAddressClass, jAddress, "own", 1L);
        setStringField(env, WalletAddressClass, jAddress, "token", token);
    }

    return jAddress;
 }

JNIEXPORT jobject JNICALL BEAM_JAVA_API_INTERFACE(createWallet)(JNIEnv *env, jobject thiz, 
    jstring appVersion, jstring nodeAddrStr, jstring appDataStr, jstring passStr, jstring phrasesStr, jboolean restore)
{
    auto appData = JString(env, appDataStr).value();

    initLogger(appData, JString(env, appVersion).value());
    
    LOG_DEBUG() << "creating wallet...";

    auto pass = JString(env, passStr).value();

    SecString seed;
    
    {
        std::string st = JString(env, phrasesStr).value();

        boost::algorithm::trim_if(st, [](char ch){ return ch == ';';});

        WordList phrases = string_helpers::split(st, ';');

        assert(phrases.size() == WORD_COUNT);

        if (!isValidMnemonic(phrases, language::en))
        {
            LOG_ERROR() << "Invalid seed phrase provided: " << st;
            return nullptr;
        }

        auto buf = decodeMnemonic(phrases);
        seed.assign(buf.data(), buf.size());
    }

    reactor = io::Reactor::create();
    io::Reactor::Scope scope(*reactor);

    walletDB = WalletDB::init(
        appData + "/" WALLET_FILENAME,
        pass,
        seed.hash()
    );

    if(walletDB)
    {
        LOG_DEBUG() << "wallet successfully created.";

        passwordHash.V = SecString(pass).hash().V;
        // generate default address
        
        if (restore)
        {
            nodeModel = make_unique<NodeModel>(appData);
            nodeModel->start();
            nodeModel->setKdf(walletDB->get_MasterKdf());
            nodeModel->startNode();
        }
        
        // generate default address
        WalletAddress address;
        walletDB->createAddress(address);
        address.m_label = "default";
        walletDB->saveAddress(address);
        
        if (restore)
        {
            walletModel = make_unique<WalletModel>(walletDB, "127.0.0.1:10005", reactor);
        }
        else
        {
            walletModel = make_unique<WalletModel>(walletDB, JString(env, nodeAddrStr).value(), reactor);
        }

        jobject walletObj = env->AllocObject(WalletClass);

        auto pushTxCreator = std::make_shared<lelantus::PushTransaction::Creator>(walletDB);
        auto pullTxCreator = std::make_shared<lelantus::PullTransaction::Creator>();
        auto unlinkTxCreator = std::make_shared<lelantus::UnlinkFundsTransaction::Creator>();
        
        auto additionalTxCreators = std::make_shared<std::unordered_map<TxType, BaseTransaction::Creator::Ptr>>();
        additionalTxCreators->emplace(TxType::PushTransaction, pushTxCreator);
        additionalTxCreators->emplace(TxType::PullTransaction, pullTxCreator);
        additionalTxCreators->emplace(TxType::UnlinkFunds, unlinkTxCreator);
        
        
        walletModel->start(initNotifications(false), true, additionalTxCreators);

        return walletObj;
    }

    LOG_ERROR() << "wallet creation error.";

    return nullptr;
}

JNIEXPORT jboolean JNICALL BEAM_JAVA_API_INTERFACE(isWalletInitialized)(JNIEnv *env, jobject thiz, 
    jstring appData)
{
    LOG_DEBUG() << "checking if wallet exists...";

    return WalletDB::isInitialized(JString(env, appData).value() + "/" WALLET_FILENAME) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL BEAM_JAVA_API_INTERFACE(closeWallet)(JNIEnv *env, jobject thiz)
{
    LOG_DEBUG() << "close wallet if it exists";

    if (nodeModel)
    {
        nodeModel.reset();
    }

    if (walletModel)
    {
        walletModel.reset();
    }
}

JNIEXPORT jboolean JNICALL BEAM_JAVA_API_INTERFACE(isWalletRunning)(JNIEnv *env, jobject thiz)
{
    return walletModel != nullptr;
}

JNIEXPORT jobject JNICALL BEAM_JAVA_API_INTERFACE(openWallet)(JNIEnv *env, jobject thiz, 
    jstring appVersion, jstring nodeAddrStr, jstring appDataStr, jstring passStr)
{
    auto appData = JString(env, appDataStr).value();

    initLogger(appData, JString(env, appVersion).value());

    LOG_DEBUG() << "opening wallet...";

    string pass = JString(env, passStr).value();
    
    reactor = io::Reactor::create();
    io::Reactor::Scope scope(*reactor);
    
    walletDB = WalletDB::open(appData + "/" WALLET_FILENAME, pass);

    if(walletDB)
    {
        LOG_DEBUG() << "wallet successfully opened.";

        passwordHash.V = SecString(pass).hash().V;
        
        walletModel = make_unique<WalletModel>(walletDB, JString(env, nodeAddrStr).value(), reactor);
                
        jobject walletObj = env->AllocObject(WalletClass);

        auto pushTxCreator = std::make_shared<lelantus::PushTransaction::Creator>(walletDB);
        auto pullTxCreator = std::make_shared<lelantus::PullTransaction::Creator>();
        auto unlinkTxCreator = std::make_shared<lelantus::UnlinkFundsTransaction::Creator>();
        
        auto additionalTxCreators = std::make_shared<std::unordered_map<TxType, BaseTransaction::Creator::Ptr>>();
        additionalTxCreators->emplace(TxType::PushTransaction, pushTxCreator);
        additionalTxCreators->emplace(TxType::PullTransaction, pullTxCreator);
        additionalTxCreators->emplace(TxType::UnlinkFunds, unlinkTxCreator);
        
        walletModel->start(initNotifications(false), true, additionalTxCreators);

        return walletObj;
    }

    LOG_ERROR() << "wallet not opened.";

    return nullptr;
}

JNIEXPORT jobject JNICALL BEAM_JAVA_API_INTERFACE(createMnemonic)(JNIEnv *env, jobject thiz)
{
    auto phrases = createMnemonic(getEntropy(), language::en);

    jobjectArray phrasesArray = env->NewObjectArray(static_cast<jsize>(phrases.size()), env->FindClass("java/lang/String"), 0);

    int i = 0;
    for (auto& phrase : phrases)
    {
        jstring str = env->NewStringUTF(phrase.c_str());
        env->SetObjectArrayElement(phrasesArray, i++, str);
        env->DeleteLocalRef(str);
    }

    return phrasesArray;
}

JNIEXPORT jobject JNICALL BEAM_JAVA_API_INTERFACE(getDictionary)(JNIEnv *env, jobject thiz)
{
    //auto phrases = beam::createMnemonic(beam::getEntropy(), beam::language::en);

    jobjectArray dictionary = env->NewObjectArray(static_cast<jsize>(language::en.size()), env->FindClass("java/lang/String"), 0);

    int i = 0;
    for (auto& word : language::en)
    {
        jstring str = env->NewStringUTF(word);
        env->SetObjectArrayElement(dictionary, i++, str);
        env->DeleteLocalRef(str);
    }

    return dictionary;
}

JNIEXPORT jobject JNICALL BEAM_JAVA_API_INTERFACE(getDefaultPeers)(JNIEnv *env, jobject thiz)
{
    auto peers = getDefaultPeers();

    jobjectArray peersArray = env->NewObjectArray(static_cast<jsize>(peers.size()), env->FindClass("java/lang/String"), 0);

    int i = 0;
    for (auto& peer : peers)
    {
        jstring str = env->NewStringUTF(peer.c_str());
        env->SetObjectArrayElement(peersArray, i++, str);
        env->DeleteLocalRef(str);
    }

    return peersArray;
}

JNIEXPORT jboolean JNICALL BEAM_JAVA_API_INTERFACE(checkReceiverAddress)(JNIEnv *env, jobject thiz, jstring address)
{
    auto str = JString(env, address).value();

    return CheckReceiverAddress(str);
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(getWalletStatus)(JNIEnv *env, jobject thiz)
{
    LOG_DEBUG() << "getWalletStatus()";

    walletModel->getAsync()->getWalletStatus();
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(getTransactions)(JNIEnv *env, jobject thiz)
{
    LOG_DEBUG() << "getTransactions()";

    walletModel->getAsync()->getTransactions();
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(getUtxosStatus)(JNIEnv *env, jobject thiz)
{
    LOG_DEBUG() << "getUtxosStatus()";

    walletModel->getAsync()->getUtxosStatus();
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(syncWithNode)(JNIEnv *env, jobject thiz)
{
    LOG_DEBUG() << "syncWithNode()";

    walletModel->getAsync()->syncWithNode();
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(sendTransaction)(JNIEnv *env, jobject thiz,
    jstring senderAddr, jstring receiverAddr, jstring comment, jlong amount, jlong fee)
{
    LOG_DEBUG() << "sendMoneyToToken(" << JString(env, senderAddr).value() << ", " << JString(env, receiverAddr).value() << ", " << JString(env, comment).value() << ", " << amount << ", " << fee << ")";

    auto txParameters = beam::wallet::ParseParameters(JString(env, receiverAddr).value());
    if (!txParameters)
    {
        return;
    }

    auto messageString = JString(env, comment).value();
    
    uint64_t bAmount = round(amount * Rules::Coin);
    uint64_t bfee = fee;
    
    auto peer = txParameters->GetParameter<beam::wallet::WalletID>(beam::wallet::TxParameterID::PeerID);
        
    auto p = beam::wallet::CreateSimpleTransactionParameters()
    .SetParameter(beam::wallet::TxParameterID::Amount, bAmount)
    .SetParameter(beam::wallet::TxParameterID::Fee, bfee)
    .SetParameter(beam::wallet::TxParameterID::Message, beam::ByteBuffer(messageString.begin(), messageString.end()));

    WalletID fromID(Zero);
    fromID.FromHex(JString(env, senderAddr).value());
    p.SetParameter(beam::wallet::TxParameterID::PeerID, peer);
    p.SetParameter(beam::wallet::TxParameterID::MyID, fromID);

    auto params = beam::wallet::ParseParameters(JString(env, receiverAddr).value());
    bool isToken =  params && params->GetParameter<beam::wallet::TxType>(beam::wallet::TxParameterID::TransactionType);

    if (isToken)
    {
        p.SetParameter(beam::wallet::TxParameterID::OriginalToken, JString(env, receiverAddr).value());
    }
    
    auto identity = txParameters->GetParameter<beam::PeerID>(beam::wallet::TxParameterID::PeerWalletIdentity);
    
    if (identity)
    {
        p.SetParameter(beam::wallet::TxParameterID::PeerWalletIdentity, *identity);
    }
    
    walletModel->getAsync()->startTransaction(std::move(p));
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(sendMoney)(JNIEnv *env, jobject thiz,
    jstring senderAddr, jstring receiverAddr, jstring comment, jlong amount, jlong fee)
{
    LOG_DEBUG() << "sendMoney(" << JString(env, senderAddr).value() << ", " << JString(env, receiverAddr).value() << ", " << JString(env, comment).value() << ", " << amount << ", " << fee << ")";

    WalletID receiverID(Zero);
    receiverID.FromHex(JString(env, receiverAddr).value());

    auto sender = JString(env, senderAddr).value();

    if (sender.empty())
    {
        walletModel->getAsync()->sendMoney(receiverID
            , JString(env, comment).value()
            , Amount(amount)
            , Amount(fee));
    }
    else
    {
        WalletID senderID(Zero);
        senderID.FromHex(sender);

        walletModel->getAsync()->sendMoney(senderID, receiverID
            , JString(env, comment).value()
            , Amount(amount)
            , Amount(fee));
    }
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(calcChange)(JNIEnv *env, jobject thiz,
    jlong amount)
{
    LOG_DEBUG() << "calcChange(" << amount << ")";

    walletModel->getAsync()->calcChange(Amount(amount));
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(getAddresses)(JNIEnv *env, jobject thiz,
    jboolean own)
{
    LOG_DEBUG() << "getAddresses(" << own << ")";

    walletModel->getAsync()->getAddresses(own);
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(generateNewAddress)(JNIEnv *env, jobject thiz)
{
    LOG_DEBUG() << "generateNewAddress()";

    walletModel->getAsync()->generateNewAddress();
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(saveAddress)(JNIEnv *env, jobject thiz,
    jobject walletAddrObj, jboolean own)
{
    LOG_DEBUG() << "saveAddress()";

    WalletAddress addr;

    addr.m_walletID.FromHex(getStringField(env, WalletAddressClass, walletAddrObj, "walletID"));
    addr.m_label = getStringField(env, WalletAddressClass, walletAddrObj, "label");
    addr.m_category = getStringField(env, WalletAddressClass, walletAddrObj, "category");
    addr.m_createTime = getLongField(env, WalletAddressClass, walletAddrObj, "createTime");
    addr.m_duration = getLongField(env, WalletAddressClass, walletAddrObj, "duration");
    addr.m_OwnID = getLongField(env, WalletAddressClass, walletAddrObj, "own");

    walletModel->getAsync()->saveAddress(addr, own);
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(importRecovery)(JNIEnv *env, jobject thiz, jstring jpath)
{
    auto path = JString(env, jpath).value();

    LOG_DEBUG() << "importRecovery path = " << path;

    walletModel->getAsync()->importRecovery(path);
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(updateAddress)(JNIEnv *env, jobject thiz,
    jstring addr, jstring name, jint addressExpirationEnum)
{
    WalletID walletID(Zero);

    if (!walletID.FromHex(JString(env, addr).value()))
    {
        LOG_ERROR() << "Address is not valid!!!";

        return;
    }

    WalletAddress::ExpirationStatus expirationStatus;
    switch (addressExpirationEnum)
    {
    case 0:
        expirationStatus = WalletAddress::ExpirationStatus::Expired;
        break;
    case 1:
        expirationStatus = WalletAddress::ExpirationStatus::OneDay;
        break;
    case 2:
        expirationStatus = WalletAddress::ExpirationStatus::Never;
        break;
    
    default:
        LOG_ERROR() << "Address expiration is not valid!!!";
        return;
    }

    walletModel->getAsync()->updateAddress(walletID, JString(env, name).value(), expirationStatus);
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(saveAddressChanges)(JNIEnv *env, jobject thiz,
    jstring addr, jstring name, jboolean isNever, jboolean makeActive, jboolean makeExpired)
{
    WalletID walletID(Zero);

    if (!walletID.FromHex(JString(env, addr).value()))
    {
        LOG_ERROR() << "Address is not valid!!!";
        return;
    }

    WalletAddress::ExpirationStatus expirationStatus;
    if (isNever)
        expirationStatus = WalletAddress::ExpirationStatus::Never;
    else if (makeActive)
        expirationStatus = WalletAddress::ExpirationStatus::OneDay;
    else if (makeExpired)
		expirationStatus = WalletAddress::ExpirationStatus::Expired;
    else
    {
        LOG_ERROR() << "Address expiration is not valid!!!";
        return;
    }

    walletModel->getAsync()->updateAddress(walletID, JString(env, name).value(), expirationStatus);
}

// don't use it. i don't check it
JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(cancelTx)(JNIEnv *env, jobject thiz,
    jstring txId)
{
    LOG_DEBUG() << "cancelTx()";

    auto buffer = from_hex(JString(env, txId).value());
    TxID id;

    std::copy_n(buffer.begin(), id.size(), id.begin());
    walletModel->getAsync()->cancelTx(id);
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(deleteTx)(JNIEnv *env, jobject thiz,
    jstring txId)
{
    LOG_DEBUG() << "deleteTx()";

    auto buffer = from_hex(JString(env, txId).value());
    TxID id;

    std::copy_n(buffer.begin(), id.size(), id.begin());
    walletModel->getAsync()->deleteTx(id);
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(deleteAddress)(JNIEnv *env, jobject thiz,
    jstring walletID)
{
    WalletID id(Zero);

    if (!id.FromHex(JString(env, walletID).value()))
    {
        LOG_ERROR() << "Address is not valid!!!";

        return;
    }
    walletModel->getAsync()->deleteAddress(id);
}

JNIEXPORT jboolean JNICALL BEAM_JAVA_WALLET_INTERFACE(checkWalletPassword)(JNIEnv *env, jobject thiz,
    jstring password)
{
    auto pass = JString(env, password).value();
    auto hash = SecString(pass).hash();

    return passwordHash.V == hash.V;
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(changeWalletPassword)(JNIEnv *env, jobject thiz,
    jstring password)
{
    auto pass = JString(env, password).value();

    passwordHash.V = SecString(pass).hash().V;
    walletModel->getAsync()->changeWalletPassword(pass);
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(getPaymentInfo)(JNIEnv *env, jobject thiz,
    jstring txID)
{
    auto buffer = from_hex(JString(env, txID).value());
    TxID id;

    std::copy_n(buffer.begin(), id.size(), id.begin());

    walletModel->getAsync()->exportPaymentProof(id);
}

JNIEXPORT jobject JNICALL BEAM_JAVA_WALLET_INTERFACE(verifyPaymentInfo)(JNIEnv *env, jobject thiz,
    jstring rawPaymentInfo)
{
    string str = JString(env, rawPaymentInfo).value();
    storage::PaymentInfo paymentInfo;
    try
    {
        paymentInfo = storage::PaymentInfo::FromByteBuffer(from_hex(str));
    }
    catch (...)
    {
        paymentInfo.Reset();
    }

    jobject jPaymentInfo = env->AllocObject(PaymentInfoClass);

    {
        setStringField(env, PaymentInfoClass, jPaymentInfo, "senderId", to_string(paymentInfo.m_Sender));
        setStringField(env, PaymentInfoClass, jPaymentInfo, "receiverId", to_string(paymentInfo.m_Receiver));
        setLongField(env, PaymentInfoClass, jPaymentInfo, "amount", paymentInfo.m_Amount);
        setStringField(env, PaymentInfoClass, jPaymentInfo, "kernelId", to_string(paymentInfo.m_KernelID));
        setBooleanField(env, PaymentInfoClass, jPaymentInfo, "isValid", paymentInfo.IsValid());
        setStringField(env, PaymentInfoClass, jPaymentInfo, "rawProof", str);
    }

    return jPaymentInfo;
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(getCoinsByTx)(JNIEnv *env, jobject thiz,
    jstring txID)
{
    auto buffer = from_hex(JString(env, txID).value());
    TxID id;

    std::copy_n(buffer.begin(), id.size(), id.begin());

    walletModel->getAsync()->getCoinsByTx(id);
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(changeNodeAddress)(JNIEnv *env, jobject thiz,
    jstring address)
{
    auto addr = JString(env, address).value();

    walletModel->getAsync()->setNodeAddress(addr);
}

JNIEXPORT jstring JNICALL BEAM_JAVA_WALLET_INTERFACE(exportOwnerKey)(JNIEnv *env, jobject thiz,
    jstring pass)
{
    std::string ownerKey = walletModel->exportOwnerKey(JString(env, pass).value());
    return env->NewStringUTF(ownerKey.c_str());
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(importDataFromJson)(JNIEnv *env, jobject thiz,
    jstring jdata)
{
    auto data = JString(env, jdata).value();

    walletModel->getAsync()->importDataFromJson(data);
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(exportDataToJson)(JNIEnv *env, jobject thiz)
{
    walletModel->getAsync()->exportDataToJson();
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(switchOnOffExchangeRates)(JNIEnv *env, jobject thiz, jboolean isActive)
{
    walletModel->getAsync()->switchOnOffExchangeRates(isActive);
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(switchOnOffNotifications)(JNIEnv *env, jobject thiz,
    jint notificationTypeEnum, jboolean isActive)
{
    if (notificationTypeEnum <= static_cast<int>(Notification::Type::SoftwareUpdateAvailable)
     || notificationTypeEnum > static_cast<int>(Notification::Type::TransactionCompleted))
    {
        LOG_ERROR() << "Notification type is not valid!!!";
    }
    
    walletModel->getAsync()->switchOnOffNotifications(static_cast<Notification::Type>(notificationTypeEnum), isActive);
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(getNotifications)(JNIEnv *env, jobject thiz)
{
    walletModel->getAsync()->getNotifications();
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(markNotificationAsRead)(JNIEnv *env, jobject thiz, jstring idString)
{
    auto buffer = from_hex(JString(env, idString).value());
    Blob rawData(buffer.data(), static_cast<uint32_t>(buffer.size()));
    ECC::uintBig id(rawData);

    walletModel->getAsync()->markNotificationAsRead(id);
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(deleteNotification)(JNIEnv *env, jobject thiz, jstring idString)
{
    auto buffer = from_hex(JString(env, idString).value());
    Blob rawData(buffer.data(), static_cast<uint32_t>(buffer.size()));
    ECC::uintBig id(rawData);

    walletModel->getAsync()->deleteNotification(id);
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(getExchangeRates)(JNIEnv *env, jobject thiz)
{
    walletModel->getAsync()->getExchangeRates();
}

JNIEXPORT void JNICALL BEAM_JAVA_WALLET_INTERFACE(setCoinConfirmationsOffset)(JNIEnv *env, jobject thiz, jlong offset)
{
    walletModel->setCoinConfirmationsOffset(offset);
}

JNIEXPORT jlong JNICALL BEAM_JAVA_WALLET_INTERFACE(getCoinConfirmationsOffset)(JNIEnv *env, jobject thiz)
{
    return walletModel->getCoinConfirmationsOffset();
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
    JNIEnv *env;
    JVM = vm;

    JVM->GetEnv((void**) &env, JNI_VERSION_1_6);

    Android_JNI_getEnv();

    {
        jclass cls = env->FindClass(BEAM_JAVA_PATH "/listeners/WalletListener");
        WalletListenerClass = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
    }

    {
        jclass cls = env->FindClass(BEAM_JAVA_PATH "/entities/Wallet");
        WalletClass = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
    }

    {
        jclass cls = env->FindClass(BEAM_JAVA_PATH "/entities/dto/WalletStatusDTO");
        WalletStatusClass = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
    }

    {
        jclass cls = env->FindClass(BEAM_JAVA_PATH "/entities/dto/SystemStateDTO");
        SystemStateClass = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
    }

    {
        jclass cls = env->FindClass(BEAM_JAVA_PATH "/entities/dto/TxDescriptionDTO");
        TxDescriptionClass = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
    }

    {
        jclass cls = env->FindClass(BEAM_JAVA_PATH "/entities/dto/UtxoDTO");
        UtxoClass = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
    }

    {
        jclass cls = env->FindClass(BEAM_JAVA_PATH "/entities/dto/WalletAddressDTO");
        WalletAddressClass = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
    }

    {
        jclass cls = env->FindClass(BEAM_JAVA_PATH "/entities/dto/PaymentInfoDTO");
        PaymentInfoClass = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
    }

    {
        jclass cls = env->FindClass(BEAM_JAVA_PATH "/entities/dto/ExchangeRateDTO");
        ExchangeRateClass = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
    }

    {
        jclass cls = env->FindClass(BEAM_JAVA_PATH "/entities/dto/NotificationDTO");
        NotificationClass = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
    }

    {
        jclass cls = env->FindClass(BEAM_JAVA_PATH "/entities/dto/VersionInfoDTO");
        VersionInfoClass = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
    }

    return JNI_VERSION_1_6;
}

#ifdef __cplusplus
}
#endif