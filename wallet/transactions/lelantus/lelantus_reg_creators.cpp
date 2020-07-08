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
#include "lelantus_reg_creators.h"
#include "unlink_transaction.h"
#include "push_transaction.h"
#include "pull_transaction.h"

namespace beam::wallet::lelantus
{
    void RegisterCreators(Wallet& wallet, bool withAssets)
    {
        wallet.RegisterTransactionType(TxType::UnlinkFunds, std::make_shared<UnlinkFundsTransaction::Creator>(withAssets));
        wallet.RegisterTransactionType(TxType::PushTransaction, std::make_shared<PushTransaction::Creator>(withAssets));
        wallet.RegisterTransactionType(TxType::PullTransaction, std::make_shared<PullTransaction::Creator>(withAssets));
    }
}