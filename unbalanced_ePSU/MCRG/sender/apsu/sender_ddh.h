// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.
/** @file
*****************************************************************************
This is an implementation of membership conditional randomness generation (MCRG).

References:
[TBZCC-USENIX-2025]: Fast Enhanced Private Set Union in the Balanced and Unbalanced Scenarios
Binbin Tu, Yujie Bai, Cong Zhang, Yang Cao, Yu Chen
USENIX Security 2025,

Modified from the following project:
<https://github.com/real-world-cryprography/APSU>

With modifications:
1. Remove the ddh-peqt;
2. Write the immediate matrix into files.

*****************************************************************************
* @author developed by Yujie Bai and Yang Cao (modified by Yu Chen)
*****************************************************************************/

#pragma once

// STD
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>

// APSU
#include "apsu/crypto_context.h"
#include "apsu/item.h"
#include "apsu/itt.h"
#include "apsu/match_record.h"
#include "apsu/network/channel.h"
#include "apsu/network/network_channel.h"
#include "apsu/oprf/oprf_receiver.h"
#include "apsu/powers.h"
#include "apsu/psu_params.h"
#include "apsu/requests.h"
#include "apsu/responses.h"
#include "apsu/seal_object.h"
// #include "apsu/permute/apsu_OSNSender.h"

// libOTe
#include <cryptoTools/Network/Session.h>
#include <cryptoTools/Network/Channel.h>
#include <cryptoTools/Network/IOService.h>
#include <cryptoTools/Common/Timer.h>
#include "cryptoTools/Common/Defines.h"
#include "cryptoTools/Common/BitVector.h"
#include "cryptoTools/Network/Channel.h"
#include "libOTe/TwoChooseOne/Iknp/IknpOtExtSender.h"
#include "libOTe/TwoChooseOne/Iknp/IknpOtExtReceiver.h"
#include "libOTe/Base/BaseOT.h"

#include "coproto/Socket/AsioSocket.h"

namespace apsu {
    namespace sender {
        /**
        The Receiver class implements all necessary functions to create and send parameter, OPRF,
        and PSU or labeled PSU queries (depending on the sender), and process any responses
        received. Most of the member functions are static, but a few (related to creating and
        processing the query itself) require an instance of the class to be created.

        The class includes two versions of an API to performs the necessary operations. The "simple"
        API consists of three functions: Receiver::RequestParams, Receiver::RequestOPRF, and
        Receiver::request_query. However, these functions only support network::NetworkChannel, such
        as network::ZMQChannel, for the communication. Other channels, such as
        network::StreamChannel, are only supported by the "advanced" API.

        The advanced API requires many more steps. The full process is as follows:

        (0 -- optional) Receiver::CreateParamsRequest must be used to create a parameter request.
        The request must be sent to the sender on a channel with network::Channel::send. The sender
        must respond to the request and the response must be received on the channel with
        network::Channel::receive_response. The received Response object must be converted to the
        right type (ParamsResponse) with the to_params_response function. This function will return
        nullptr if the received response was not of the right type. A PSUParams object can be
        extracted from the response.

        (1) A Receiver object must be created from a PSUParams object. The PSUParams must match what
        the sender uses.

        (2) Receiver::CreateOPRFReceiver must be used to process the input vector of items and
        return an associated oprf::OPRFReceiver object. Next, Receiver::CreateOPRFRequest must be
        used to create an OPRF request from the oprf::OPRFReceiver, which can subsequently be sent
        to the sender with network::Channel::send. The sender must respond to the request and the
        response must be received on the channel with network::Channel::receive_response. The
        received Response object must be converted to the right type (OPRFResponse) with the
        to_oprf_response function. This function will return nullptr if the received response was
        not of the right type. Finally, Receiver::ExtractHashes must be called with the
        OPRFResponse and the oprf::OPRFReceiver object. This function returns
        std::pair<std::vector<HashedItem>, std::vector<LabelKey>>, containing the OPRF hashed items
        and the label encryption keys. Both vectors in this pair must be kept for the next steps.

        (3) Receiver::create_query (non-static member function) must then be used to create the
        query itself. The function returns std::pair<Request, IndexTranslationTable>, where the
        Request object contains the query itself to be send to the sender, and the
        IndexTranslationTable is an object associated to this query describing how the internal data
        structures of the query maps to the vector of OPRF hashed items given to
        Receiver::create_query. The IndexTranslationTable object is needed later to process the
        responses from the sender. The Request object must be sent to the sender with
        network::Channel::send. The received Response object must be converted to the right type
        (QueryResponse) with the to_query_response function. This function will return nullptr if
        the received response was not of the right type. The QueryResponse contains only one
        important piece of data: the number of ResultPart objects the receiver should expect to
        receive from the sender in the next step.

        (4) network::Channel::receive_result must be called repeatedly to receive all ResultParts.
        For each received ResultPart Receiver::process_result_part must be called to find a
        std::vector<MatchRecord> representing the match data associated to that ResultPart.
        Alternatively, one can first retrieve all ResultParts, collect them into a
        std::vector<ResultPart>, and use Receiver::process_result to find the complete result --
        just like what the simple API returns. Both Receiver::process_result_part and
        Receiver::process_result require the IndexTranslationTable and the std::vector<LabelKey>
        objects created in the previous steps.
        */
        class Sender {
        public:
            /**
            Indicates the number of random-walk steps used by the Kuku library to insert items into
            the cuckoo hash table. Increasing this number can yield better packing rates in cuckoo
            hashing.
            */
            static constexpr std::uint64_t cuckoo_table_insert_attempts = 500;

            /**
            Creates a new receiver with parameters specified. In this case the receiver has
            specified the parameters and expects the sender to use the same set.
            */
            Sender(PSUParams params);
            ~Sender(){};
            /**
            Generates a new set of keys to use for queries.
            */
            void reset_keys();

            /**
            Returns a reference to the PowersDag configured for this Receiver.
            */
            const PowersDag &get_powers_dag() const
            {
                return pd_;
            }

            /**
            Returns a reference to the CryptoContext for this Receiver.
            */
            const CryptoContext &get_crypto_context() const
            {
                return crypto_context_;
            }

            /**
            Returns a reference to the SEALContext for this Receiver.
            */
            std::shared_ptr<seal::SEALContext> get_seal_context() const
            {
                return crypto_context_.seal_context();
            }

            /**
            Performs a parameter request and returns the received PSUParams object.
            */
            static PSUParams RequestParams(network::NetworkChannel &chl);


        
            void request_query(
                const std::vector<HashedItem> &items,
                network::NetworkChannel &chl,
                const std::vector<std::string> &origin_item,
                coproto::AsioSocket SenderKKRTSocket
                );

            /**
            Creates and returns a parameter request that can be sent to the sender with the
            Receiver::SendRequest function.
            */
            static Request CreateParamsRequest();


            /**
            Creates a Query object from a vector of OPRF hashed items. The query contains the query
            request that can be extracted with the Query::extract_request function and sent to the
            sender with Receiver::SendRequest. It also contains an index translation table that
            keeps track of the order of the hashed items vector, and is used internally by the
            Receiver::process_result_part function to sort the results in the correct order.
            */
            std::pair<Request, IndexTranslationTable> create_query(
                const std::vector<HashedItem> &items,
                const std::vector<std::string> &origin_item,
                coproto::AsioSocket SenderKKRTSocket);

            /**
            Processes a ResultPart object and returns a vector of MatchRecords in the same order as
            the original vector of OPRF hashed items used to create the query. The return value
            includes matches only for those items whose results happened to be in this particular
            result part. Thus, to determine whether there was a match with the sender's data, the
            results for each received ResultPart must be checked.
            */
            void process_result_part(
                
                const IndexTranslationTable &itt,
                const ResultPart &result_part,
                network::NetworkChannel &chl) const;

            /**
            This function does multiple calls to Receiver::process_result_part, once for each
            ResultPart in the given vector. The results are collected together so that the returned
            vector of MatchRecords reflects the logical OR of the results from each ResultPart.
            */
         /*   std::vector<MatchRecord> process_result(
                const std::vector<LabelKey> &label_keys,
                const IndexTranslationTable &itt,
                const std::vector<ResultPart> &result) const;*/
            
            /**
             * @brief send items values to sender to finished the complete APSU
             * 
             * @param[in] conn_addr sender web socker
             */

            // void ResponseOT(std::string conn_addr);

// #if CARDSUM == 1
//             void Cardsum_Send();
// #endif
        private:
            /**
            Recomputes the PowersDag. The function returns the depth of the PowersDag. In some cases
            the receiver may want to ensure that the depth of the powers computation will be as
            expected (PowersDag::depth), and otherwise attempt to reconfigure the PowersDag.
            */
            std::uint32_t reset_powers_dag(const std::set<std::uint32_t> &source_powers);

            void process_result_worker(
                std::atomic<std::uint32_t> &package_count,
                const IndexTranslationTable &itt,
                network::NetworkChannel &chl);

            void initialize();
            // params for permutation 
            std::vector<uint64_t > permutation;
            std::vector<uint64_t> sender_set;
            std::vector<std::vector<oc::block > > psu_result_before_shuffle;
            int send_size=0,receiver_size =0;
            //std::vector<vector<uint64_t> > psu_result_before_shuffle;
            PSUParams params_;

            CryptoContext crypto_context_;

            PowersDag pd_;

            SEALObject<seal::RelinKeys> relin_keys_;

            oc::Timer all_timer;
            oc::PRNG prng;
            std::vector<oc::block> cuckoo_item;
            std::vector<oc::block> shuffle_item;

// #if ARBITARY == 0 
//            std::vector<std::array<oc::block, 2>> sendMessages;
//            std::vector<std::array<oc::block, 2>> shuffleMessages;
// #else
//            std::vector<std::vector<std::array<oc::block, 2> > > sendMessages;
//            std::vector<std::vector<std::array<oc::block, 2> > >shuffleMessages;
//            size_t item_len = 0;
// #endif
// #if CARDSUM == 1
//             std::vector<uint64_t> valueMessages;
//             std::vector<uint64_t> shuffle_valueMessages;
// #endif

        }; // class Receiver
    }      // namespace receiver
} // namespace apsu
