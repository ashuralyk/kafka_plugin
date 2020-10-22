/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */
//
#include <stdlib.h>
#include <eosio/kafka_plugin/kafka_producer.hpp>
#include <eosio/kafka_plugin/kafka_plugin.hpp>

#include <eosio/http_plugin/http_plugin.hpp>
#include <eosio/chain/eosio_contract.hpp>
#include <eosio/chain/config.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/transaction.hpp>
#include <eosio/chain/types.hpp>

#include <fc/io/json.hpp>
#include <fc/utf8.hpp>
#include <fc/variant.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/chrono.hpp>
#include <boost/signals2/connection.hpp>

#include <queue>

namespace {
    template <typename T>
    T parse_body(const std::string &body)
    {
        if (body.empty())
        {
            EOS_THROW(eosio::chain::invalid_http_request, "A Request body is required");
        }

        try
        {
            try
            {
                return fc::json::from_string(body).as<T>();
            }
            catch (const eosio::chain::chain_exception &e)
            {
                throw fc::exception(e);
            }
        }
        EOS_RETHROW_EXCEPTIONS(eosio::chain::invalid_http_request, "Unable to parse valid input from POST body");
    }
}

namespace fc { class variant; }

namespace eosio {

    using chain::account_name;
    using chain::action_name;
    using chain::block_id_type;
    using chain::permission_name;
    using chain::transaction;
    using chain::signed_transaction;
    using chain::signed_block;
    using chain::transaction_id_type;
    using chain::packed_transaction;
    using chain::permission_level;

    static appbase::abstract_plugin &_kafka_plugin = app().register_plugin<kafka_plugin>();
    using kafka_producer_ptr = std::shared_ptr<class kafka_producer>;

    class kafka_plugin_impl {
    public:
        kafka_plugin_impl();

        ~kafka_plugin_impl();

        fc::optional<boost::signals2::scoped_connection> accepted_block_connection;
        fc::optional<boost::signals2::scoped_connection> irreversible_block_connection;
        fc::optional<boost::signals2::scoped_connection> accepted_transaction_connection;
        fc::optional<boost::signals2::scoped_connection> applied_transaction_connection;
        chain_plugin *chain_plug;
        struct action_info {
            uint64_t global_sequence;
            fc::unsigned_int action_ordinal;
            account_name receiver;
            account_name account;
            action_name name;
            vector<permission_level> authorization;
            string data;
        };

        struct transaction_info {
            transaction_id_type id;
            uint32_t block_number;
            chain::block_timestamp_type block_time;
            fc::optional<chain::transaction_receipt_header> receipt;
            vector<action_info> actions;
        };

        struct set_log_filter_params {
            bool prev;
            vector<account_name> accounts;
        };

        void consume_blocks();

        void accepted_block(const chain::block_state_ptr &);

        void applied_irreversible_block(const chain::block_state_ptr &);

        void accepted_transaction(const chain::transaction_metadata_ptr &);

        void applied_transaction(const chain::transaction_trace_ptr &);

        void process_accepted_transaction(const chain::transaction_metadata_ptr &);

        void _process_accepted_transaction(const chain::transaction_metadata_ptr &);

        void process_applied_transaction(const chain::transaction_trace_ptr &);

        void _process_applied_transaction(const chain::transaction_trace_ptr &);

        void process_accepted_block(const chain::block_state_ptr &);

        void _process_accepted_block(const chain::block_state_ptr &);

        void process_irreversible_block(const chain::block_state_ptr &);

        void _process_irreversible_block(const chain::block_state_ptr &);

        void init();

        bool configured{false};

        void filter_traction_trace(const chain::transaction_trace_ptr trace, action_name act_name);

        string transform_transaction_trace(const chain::transaction_trace_ptr &trace);

        void _process_trace(vector<chain::action_trace>::iterator action_trace_ptr, action_name act_name);

        void set_log_filter( string &&body, url_response_callback &&cb );

        template<typename Queue, typename Entry>
        void queue(Queue &queue, const Entry &e);

        template<typename Queue>
        void queue2(Queue &queue, const Queue &e);

        uint32_t start_block_num = 0;
        uint32_t blocks_behind = 0;
        uint32_t head_blocks_num = 0;
        bool start_block_reached = false;

        size_t max_queue_size = 10000;
        int queue_sleep_time = 0;
        std::deque<chain::transaction_metadata_ptr> transaction_metadata_queue;
        std::deque<chain::transaction_metadata_ptr> transaction_metadata_process_queue;
        std::deque<chain::transaction_trace_ptr> transaction_trace_queue;
        std::deque<chain::transaction_trace_ptr> transaction_trace_process_queue;
        std::map<chain::block_id_type, std::deque<chain::transaction_trace_ptr>> transaction_trace_await_map;
        std::map<uint32_t, chain::block_id_type> block_num_id_map;
        std::deque<chain::block_state_ptr> block_state_queue;
        std::deque<chain::block_state_ptr> block_state_process_queue;
        std::deque<chain::block_state_ptr> irreversible_block_state_queue;
        std::deque<chain::block_state_ptr> irreversible_block_state_process_queue;
        std::mutex mtx;
        std::mutex mtx_await;
        std::condition_variable condition;
        std::thread consume_thread;
        std::atomic_bool done{false};
        std::atomic_bool startup{true};
        fc::optional<chain::chain_id_type> chain_id;
        fc::microseconds abi_serializer_max_time;
        // std::unordered_map<chain::transaction_id_type, std::unordered_map<fc::unsigned_int, bool>> transaction_flag;

        static const account_name newaccount;
        static const account_name setabi;

        static const std::string block_states_col;
        static const std::string blocks_col;
        static const std::string trans_col;
        static const std::string trans_traces_col;
        static const std::string actions_col;
        static const std::string accounts_col;
        kafka_producer_ptr producer;

        map<account_name, bool> prev_filter;
        map<account_name, bool> post_filter;
    };

    const account_name kafka_plugin_impl::newaccount = chain::newaccount::get_name();
    const account_name kafka_plugin_impl::setabi = chain::setabi::get_name();

    const std::string kafka_plugin_impl::block_states_col = "block_states";
    const std::string kafka_plugin_impl::blocks_col = "blocks";
    const std::string kafka_plugin_impl::trans_col = "transactions";
    const std::string kafka_plugin_impl::trans_traces_col = "transaction_traces";
    const std::string kafka_plugin_impl::actions_col = "actions";
    const std::string kafka_plugin_impl::accounts_col = "accounts";

    template<typename Queue, typename Entry>
    void kafka_plugin_impl::queue(Queue &queue, const Entry &e) {
        std::unique_lock<std::mutex> lock(mtx);
        auto queue_size = queue.size();
        if (queue_size > max_queue_size) {
            lock.unlock();
            condition.notify_one();
            queue_sleep_time += 10;
            if (queue_sleep_time > 1000)
                wlog("queue size: ${q}", ("q", queue_size));
            std::this_thread::sleep_for(std::chrono::milliseconds(queue_sleep_time));
            lock.lock();
        } else {
            queue_sleep_time -= 10;
            if (queue_sleep_time < 0) queue_sleep_time = 0;
        }
        queue.emplace_back(e);
        lock.unlock();
        condition.notify_one();
    }

    template<typename Queue>
    void kafka_plugin_impl::queue2(Queue &queue, const Queue &q) {
        std::unique_lock<std::mutex> lock(mtx);
        auto queue_size = queue.size() + q.size();
        if (queue_size > max_queue_size) {
            lock.unlock();
            condition.notify_one();
            queue_sleep_time += 10 * q.size();
            if (queue_sleep_time > 1000)
                wlog("queue size: ${q}", ("q", queue_size));
            std::this_thread::sleep_for(std::chrono::milliseconds(queue_sleep_time));
            lock.lock();
        } else {
            queue_sleep_time -= 10 * q.size();
            if (queue_sleep_time < 0) queue_sleep_time = 0;
        }
        for_each(q.begin(), q.end(), [&queue](auto &v) {
            queue.emplace_back(v);
        });
        lock.unlock();
        condition.notify_one();
    }

    void kafka_plugin_impl::accepted_transaction(const chain::transaction_metadata_ptr &t) {
        // elog(">>>> accepted_trxId = ${e}", ("e", t->packed_trx()->id()));
        try {
            queue(transaction_metadata_queue, t);
        } catch (fc::exception &e) {
            elog("FC Exception while accepted_transaction ${e}", ("e", e.to_string()));
        } catch (std::exception &e) {
            elog("STD Exception while accepted_transaction ${e}", ("e", e.what()));
        } catch (...) {
            elog("Unknown exception while accepted_transaction");
        }
    }

    void kafka_plugin_impl::applied_transaction(const chain::transaction_trace_ptr &t) {
        // elog(">>>> applied_trxId = ${e}", ("e", t->id));
        if (! t->producer_block_id.valid())
            return;
        // elog(">>>> step 1");
        try {
            // auto &chain = chain_plug->chain();
            // trasaction_info_st transactioninfo = trasaction_info_st{
            //         .block_number = t->block_num,//chain.pending_block_state()->block_num,
            //         .block_time = chain.pending_block_time(),
            //         .trace =chain::transaction_trace_ptr(t)
            // };
            // trasaction_info_st &info_t = transactioninfo;
            if (any_of(t->action_traces.begin(), t->action_traces.end(), [this](const auto &v) { return prev_filter[v.receiver]; }))
            {
                ilog(">>>> PREV tx = ${tx}", ("tx", fc::json::to_string(t, fc::time_point::maximum())));
            }
            if (t->receipt && t->receipt->status == chain::transaction_receipt_header::executed) {
                // queue(transaction_trace_queue, chain::transaction_trace_ptr(t));
                // std::unique_lock<std::mutex> lock(mtx_await);
                head_blocks_num = t->block_num;
                block_num_id_map[head_blocks_num] = t->producer_block_id;
                transaction_trace_await_map[t->producer_block_id].emplace_back(chain::transaction_trace_ptr(t));
                if (block_num_id_map.count(head_blocks_num - blocks_behind) == 0) {
                    return;
                }
                auto block_num = head_blocks_num - blocks_behind;
                const auto &block_id = block_num_id_map[block_num];
                if (transaction_trace_await_map.count(block_id)) {
                    if (chain_plug->chain().fetch_block_by_id(block_id)) {
                        queue2(transaction_trace_queue, transaction_trace_await_map[block_id]);
                    }
                    transaction_trace_await_map.erase(block_id);
                    wlog("transaction_trace_await_map size = ${s}, erased block_id = ${i}", ("s", transaction_trace_await_map.size())("i", block_id));
                }
                block_num_id_map.erase(block_num);
                wlog("block_num_id_map size = ${s}, erased num = ${n}", ("s", block_num_id_map.size())("n", block_num));
            }
        } catch (fc::exception &e) {
            elog("FC Exception while applied_transaction ${e}", ("e", e.to_string()));
        } catch (std::exception &e) {
            elog("STD Exception while applied_transaction ${e}", ("e", e.what()));
        } catch (...) {
            elog("Unknown exception while applied_transaction");
        }
    }

    void kafka_plugin_impl::applied_irreversible_block(const chain::block_state_ptr &bs) {
        try {
            queue(irreversible_block_state_queue, bs);
        } catch (fc::exception &e) {
            elog("FC Exception while applied_irreversible_block ${e}", ("e", e.to_string()));
        } catch (std::exception &e) {
            elog("STD Exception while applied_irreversible_block ${e}", ("e", e.what()));
        } catch (...) {
            elog("Unknown exception while applied_irreversible_block");
        }
    }


    void kafka_plugin_impl::accepted_block(const chain::block_state_ptr &bs) {
        try {
            // queue(block_state_queue, bs);
            std::deque<chain::transaction_trace_ptr> transaction_trace_queue_tmp;
            auto top_block_num = bs->block->block_num();
            if (block_num_id_map.count(top_block_num) > 0) {
                auto prev_block_id = block_num_id_map[top_block_num];
                if (prev_block_id != bs->block->id()) {
                    wlog("block_id switched (p: ${p}, c: ${c})", ("p", prev_block_id)("c", bs->block->id()));
                    std::unique_lock<std::mutex> lock(mtx_await);
                    transaction_trace_await_map.erase(prev_block_id);
                }
            }
            block_num_id_map[top_block_num] = bs->block->id();
            auto apply_block_num = top_block_num - blocks_behind;
            if (block_num_id_map.count(apply_block_num) > 0) {
                auto block_id = block_num_id_map[apply_block_num];
                std::unique_lock<std::mutex> lock(mtx_await);
                if (auto i = transaction_trace_await_map.find(block_id); i != transaction_trace_await_map.end()) {
                    transaction_trace_queue_tmp = move(i->second);
                    transaction_trace_await_map.erase(i);
                    lock.unlock();
                    queue2(transaction_trace_queue, transaction_trace_queue_tmp);
                    wlog("transaction_trace_await_map size = ${s}, erased block_id = ${i}", ("s", transaction_trace_await_map.size())("i", block_id));
                }
                block_num_id_map.erase(apply_block_num);
                wlog("block_num_id_map size = ${s}, erased num = ${n}", ("s", block_num_id_map.size())("n", apply_block_num));
            }
        } catch (fc::exception &e) {
            elog("FC Exception while accepted_block ${e}", ("e", e.to_string()));
        } catch (std::exception &e) {
            elog("STD Exception while accepted_block ${e}", ("e", e.what()));
        } catch (...) {
            elog("Unknown exception while accepted_block");
        }
    }

    void kafka_plugin_impl::consume_blocks() {
        try {

            while (true) {
                std::unique_lock<std::mutex> lock(mtx);
                while (transaction_metadata_queue.empty() &&
                       transaction_trace_queue.empty() &&
                       block_state_queue.empty() &&
                       irreversible_block_state_queue.empty() &&
                       !done) {
                    condition.wait(lock);
                }
                // capture for processing
                size_t transaction_metadata_size = transaction_metadata_queue.size();
                if (transaction_metadata_size > 0) {
                    transaction_metadata_process_queue = move(transaction_metadata_queue);
                    transaction_metadata_queue.clear();
                }
                size_t transaction_trace_size = transaction_trace_queue.size();
                if (transaction_trace_size > 0) {
                    transaction_trace_process_queue = move(transaction_trace_queue);
                    transaction_trace_queue.clear();
                }

                size_t block_state_size = block_state_queue.size();
                if (block_state_size > 0) {
                    block_state_process_queue = move(block_state_queue);
                    block_state_queue.clear();
                }
                size_t irreversible_block_size = irreversible_block_state_queue.size();
                if (irreversible_block_size > 0) {
                    irreversible_block_state_process_queue = move(irreversible_block_state_queue);
                    irreversible_block_state_queue.clear();
                }

                lock.unlock();

                // warn if queue size greater than 75%
                if (transaction_metadata_size > (max_queue_size * 0.75) ||
                    transaction_trace_size > (max_queue_size * 0.75) ||
                    block_state_size > (max_queue_size * 0.75) ||
                    irreversible_block_size > (max_queue_size * 0.75)) {
//            wlog("queue size: ${q}", ("q", transaction_metadata_size + transaction_trace_size ));
                } else if (done) {
                    ilog("draining queue, size: ${q}", ("q", transaction_metadata_size + transaction_trace_size));
                }

                // process transactions
                while (!transaction_metadata_process_queue.empty()) {
                    const auto &t = transaction_metadata_process_queue.front();
                    process_accepted_transaction(t);
                    transaction_metadata_process_queue.pop_front();
                }

                while (!transaction_trace_process_queue.empty()) {
                    const auto &t = transaction_trace_process_queue.front();
                    process_applied_transaction(t);
                    transaction_trace_process_queue.pop_front();
                }

                // process blocks
                while (!block_state_process_queue.empty()) {
                    const auto &bs = block_state_process_queue.front();
                    process_accepted_block(bs);
                    block_state_process_queue.pop_front();
                }

                // process irreversible blocks
                while (!irreversible_block_state_process_queue.empty()) {
                    const auto &bs = irreversible_block_state_process_queue.front();
                    process_irreversible_block(bs);
                    irreversible_block_state_process_queue.pop_front();
                }

                if (transaction_metadata_size == 0 &&
                    transaction_trace_size == 0 &&
                    block_state_size == 0 &&
                    irreversible_block_size == 0 &&
                    done) {
                    break;
                }
            }
            ilog("kafka_plugin consume thread shutdown gracefully");
        } catch (fc::exception &e) {
            elog("FC Exception while consuming block ${e}", ("e", e.to_string()));
        } catch (std::exception &e) {
            elog("STD Exception while consuming block ${e}", ("e", e.what()));
        } catch (...) {
            elog("Unknown exception while consuming block");
        }
    }


    void kafka_plugin_impl::process_accepted_transaction(const chain::transaction_metadata_ptr &t) {
        try {
            // always call since we need to capture setabi on accounts even if not storing transactions
            _process_accepted_transaction(t);
        } catch (fc::exception &e) {
            elog("FC Exception while processing accepted transaction metadata: ${e}", ("e", e.to_detail_string()));
        } catch (std::exception &e) {
            elog("STD Exception while processing accepted tranasction metadata: ${e}", ("e", e.what()));
        } catch (...) {
            elog("Unknown exception while processing accepted transaction metadata");
        }
    }

    void kafka_plugin_impl::process_applied_transaction(const chain::transaction_trace_ptr &t) {
        // elog(">>>> step 2 id = ${e}", ("e", t.trace->id));
        try {
            if (!start_block_reached) {
                if (t->block_num >= start_block_num) {
                    start_block_reached = true;
                }
            }
            if (start_block_reached) {
                _process_applied_transaction(t);
            }
        } catch (fc::exception &e) {
            elog("FC Exception while processing applied transaction trace: ${e}", ("e", e.to_detail_string()));
        } catch (std::exception &e) {
            elog("STD Exception while processing applied transaction trace: ${e}", ("e", e.what()));
        } catch (...) {
            elog("Unknown exception while processing applied transaction trace");
        }
    }


    void kafka_plugin_impl::process_irreversible_block(const chain::block_state_ptr &bs) {
        try {
            if (start_block_reached) {
                _process_irreversible_block(bs);
            }
        } catch (fc::exception &e) {
            elog("FC Exception while processing irreversible block: ${e}", ("e", e.to_detail_string()));
        } catch (std::exception &e) {
            elog("STD Exception while processing irreversible block: ${e}", ("e", e.what()));
        } catch (...) {
            elog("Unknown exception while processing irreversible block");
        }
    }

    void kafka_plugin_impl::process_accepted_block(const chain::block_state_ptr &bs) {
        try {
            if (!start_block_reached) {
                if (bs->block_num >= start_block_num) {
                    start_block_reached = true;
                }
            }
            if (start_block_reached) {
                _process_accepted_block(bs);
            }
        } catch (fc::exception &e) {
            elog("FC Exception while processing accepted block trace ${e}", ("e", e.to_string()));
        } catch (std::exception &e) {
            elog("STD Exception while processing accepted block trace ${e}", ("e", e.what()));
        } catch (...) {
            elog("Unknown exception while processing accepted block trace");
        }
    }

    void kafka_plugin_impl::_process_accepted_transaction(const chain::transaction_metadata_ptr &t) {
        const auto &trx = t->packed_trx();
        string trx_json = fc::json::to_string(trx, fc::time_point::maximum());
        //elog("trx_json: ${e}",("e",trx_json));
        producer->trx_kafka_sendmsg(KAFKA_TRX_ACCEPT, (char *) trx_json.c_str());
    }

    void kafka_plugin_impl::_process_applied_transaction(const chain::transaction_trace_ptr &t) {
        // elog(">>>> step 3");
        // uint64_t time = (t.block_time.time_since_epoch().count() / 1000);
        //elog("trxId = ${e}", ("e", t.trace->id));
        // string transaction_metadata_json =
        //         "{\"block_number\":" + std::to_string(t.block_number) + ",\"block_time\":" + std::to_string(time) +
        //         ",\"trace\":" + fc::json::to_string(t.trace, fc::time_point::maximum()).c_str() + "}";
        // producer->trx_kafka_sendmsg(KAFKA_TRX_APPLIED, (char *) transaction_metadata_json.c_str());
        // elog("transaction_metadata_json = ${e}",("e",transaction_metadata_json));

        if (producer->trx_kafka_get_topic(KAFKA_TRX_TRANSFER) != NULL) {
            // elog(">>>> step 4");
            // filter_traction_trace(t, N(transfer));
            // if (t->action_traces.size() > 0) {
            //     // elog(">>>> step 5");
            //     string transfer_json = fc::json::to_string(t, fc::time_point::maximum());
            //             // "{\"block_number\":" + std::to_string(t.block_number) + ",\"block_time\":" +
            //             // std::to_string(time) +
            //             // ",\"trace\":" + fc::json::to_string(t, fc::time_point::maximum()).c_str() + "}";
            //     auto sendRst = producer->trx_kafka_sendmsg(KAFKA_TRX_TRANSFER, (char *) transfer_json.c_str());
            //     // elog("transfer_json = ${e}, result = ${r}", ("e",transfer_json)("r", sendRst));
            // }

            string transfer_json = transform_transaction_trace(t);
            if (transfer_json.size() > 0)
            {
                if (any_of(t->action_traces.begin(), t->action_traces.end(), [this](const auto &v) { return post_filter[v.receiver]; }))
                {
                    elog(">>>> POST json = ${j}", ("j", transfer_json));
                }
                auto sendRst = producer->trx_kafka_sendmsg(KAFKA_TRX_TRANSFER, (char *) transfer_json.c_str());
            }
        }
    }

    void kafka_plugin_impl::_process_trace(vector<chain::action_trace>::iterator action_trace_ptr, action_name act_name) {
        /*auto inline_trace_ptr = action_trace_ptr->inline_traces.begin();
        for(;inline_trace_ptr!=action_trace_ptr->inline_traces.end();inline_trace_ptr++){
            //elog("inline action:");
            _process_trace(inline_trace_ptr,act_name);
        }*/

        if (action_trace_ptr->act.name == act_name) {
            //elog("act_name=${e}",("e",act_name));
            chain_apis::read_only::abi_bin_to_json_params params = {
                    .code = action_trace_ptr->act.account,
                    .action = action_trace_ptr->act.name,
                    .binargs = action_trace_ptr->act.data
            };

            auto readonly = chain_plug->get_read_only_api();
            auto Result = readonly.abi_bin_to_json(params);

            string data_str = fc::json::to_string(Result.args, fc::time_point::maximum());
            // action_info action_info1 = {
            //         .account = action_trace_ptr->act.account,
            //         .name = action_trace_ptr->act.name,
            //         .authorization = action_trace_ptr->act.authorization,
            //         .data_json = data_str
            // };

            action_trace_ptr->act.data.resize(data_str.size());
            action_trace_ptr->act.data.assign(data_str.begin(), data_str.end());
            //elog("act.data=${e}",("e",action_trace_ptr->act.data));
        }
    }

    void kafka_plugin_impl::filter_traction_trace(const chain::transaction_trace_ptr trace, action_name act_name) {
        vector<chain::action_trace>::iterator action_trace_ptr = trace->action_traces.begin();

        for (; action_trace_ptr != trace->action_traces.end();) {
            if (action_trace_ptr->act.name == act_name) {
                _process_trace(action_trace_ptr, act_name);
                action_trace_ptr++;
                continue;
            } else {
                action_trace_ptr = trace->action_traces.erase(action_trace_ptr);
            }
        }
    }

    void kafka_plugin_impl::set_log_filter( string &&body, url_response_callback &&cb )
    {
        ilog( "set_log_filter invoked" );
        auto filter = parse_body<set_log_filter_params>( body );
        if (filter.prev) {
            prev_filter.clear();
            for ( const auto &account : filter.accounts ) {
                prev_filter[account] = true;
            }
        } else {
            post_filter.clear();
            for ( const auto &account : filter.accounts ) {
                post_filter[account] = true;
            }
        }
        cb( 200, "ok" );
    }

    string kafka_plugin_impl::transform_transaction_trace(const chain::transaction_trace_ptr &trace)
    {
        transaction_info ti;
        ti.id = trace->id;
        ti.block_number = trace->block_num;
        ti.block_time = trace->block_time;
        ti.receipt = trace->receipt;
        for_each(trace->action_traces.begin(), trace->action_traces.end(), [&](const auto &at) {
            if (at.act.name != N(transfer) || at.receiver != at.act.account || at.receiver == N(eidosonecoin) || at.receiver == N(eosiopowcoin))
            {
                return;
            }

            action_info ai;
            if (at.receipt) {
                ai.global_sequence = (*at.receipt).global_sequence;
            }
            ai.action_ordinal = at.action_ordinal;
            ai.receiver = at.receiver;
            ai.account = at.act.account;
            ai.name = at.act.name;
            ai.authorization = at.act.authorization;

            auto result = chain_plug->get_read_only_api().abi_bin_to_json(chain_apis::read_only::abi_bin_to_json_params {
                .code = ai.account,
                .action = ai.name,
                .binargs = at.act.data
            });
            ai.data = fc::json::to_string(result.args, fc::time_point::maximum());
            ti.actions.emplace_back( ai );
        });
        if (ti.actions.size() > 0) {
            return fc::json::to_string(ti, fc::time_point::maximum());
        } else {
            return "";
        }
    }

    void kafka_plugin_impl::_process_accepted_block(const chain::block_state_ptr &bs) {
    }

    void kafka_plugin_impl::_process_irreversible_block(const chain::block_state_ptr &bs) {
    }

    kafka_plugin_impl::kafka_plugin_impl()
            : producer(new kafka_producer(done)) {
    }

    kafka_plugin_impl::~kafka_plugin_impl() {
        if (!startup) {
            try {
                ilog("kafka_db_plugin shutdown in process please be patient this can take a few minutes");
                done = true;
                condition.notify_one();

                consume_thread.join();
                producer->trx_kafka_destroy();
            } catch (std::exception &e) {
                elog("Exception on kafka_plugin shutdown of consume thread: ${e}", ("e", e.what()));
            }
        }
    }

    void kafka_plugin_impl::init() {
        ilog("starting kafka plugin thread");
        consume_thread = std::thread([this] { consume_blocks(); });
        startup = false;
    }

////////////
// kafka_plugin
////////////

    kafka_plugin::kafka_plugin()
            : my(new kafka_plugin_impl) {
    }

    kafka_plugin::~kafka_plugin() {
    }

    void kafka_plugin::set_program_options(options_description &cli, options_description &cfg) {
        cfg.add_options()
                ("accept_trx_topic", bpo::value<std::string>(),
                 "The topic for accepted transaction.")
                ("applied_trx_topic", bpo::value<std::string>(),
                 "The topic for appiled transaction.")
                ("transfer_trx_topic", bpo::value<std::string>(),
                 "The topic for transfer transaction.")
                ("kafka-uri,k", bpo::value<std::string>(),
                 "the kafka brokers uri, as 192.168.31.225:9092")
                ("kafka-queue-size", bpo::value<uint32_t>()->default_value(256),
                 "The target queue size between nodeos and kafka plugin thread.")
                ("kafka-block-start", bpo::value<uint32_t>()->default_value(256),
                 "If specified then only abi data pushed to kafka until specified block is reached.")
                ("kafka-block-behind", bpo::value<uint32_t>()->default_value(3),
                 "If specified then transactions will be sent to kafka behind specified block number.");
    }

    void kafka_plugin::plugin_initialize(const variables_map &options) {
        char *accept_trx_topic = NULL;
        char *applied_trx_topic = NULL;
        char *transfer_trx_topic = NULL;
        char *brokers_str = NULL;

        try {
            if (options.count("kafka-uri")) {
                brokers_str = (char *) (options.at("kafka-uri").as<std::string>().c_str());
                elog("brokers_str:${j}", ("j", brokers_str));
                if (options.count("accept_trx_topic") != 0) {
                    accept_trx_topic = (char *) (options.at("accept_trx_topic").as<std::string>().c_str());
                    elog("accept_trx_topic:${j}", ("j", accept_trx_topic));
                }
                if (options.count("applied_trx_topic") != 0) {
                    applied_trx_topic = (char *) (options.at("applied_trx_topic").as<std::string>().c_str());
                    elog("applied_trx_topic:${j}", ("j", applied_trx_topic));
                }
                if (options.count("transfer_trx_topic") != 0) {
                    transfer_trx_topic = (char *) (options.at("transfer_trx_topic").as<std::string>().c_str());
                    elog("transfer_trx_topic:${j}", ("j", transfer_trx_topic));
                }

                if (0 != my->producer->trx_kafka_init(brokers_str, accept_trx_topic, applied_trx_topic,
                                                      transfer_trx_topic)) {
                    elog("trx_kafka_init fail");
                } else {
                    elog("trx_kafka_init ok");
                }
            }

            if (options.count("kafka-uri")) {
                ilog("initializing kafka_plugin");
                my->configured = true;

                if (options.count("kafka-queue-size")) {
                    my->max_queue_size = options.at("kafka-queue-size").as<uint32_t>();
                }
                if (options.count("kafka-block-start")) {
                    my->start_block_num = options.at("kafka-block-start").as<uint32_t>();
                }
                if (options.count("kafka-block-behind")) {
                    my->blocks_behind = options.at("kafka-block-behind").as<uint32_t>();
                }
                if (my->start_block_num == 0) {
                    my->start_block_reached = true;
                }

                // hook up to signals on controller
                //chain_plugin* chain_plug = app().find_plugiin<chain_plugin>();
                my->chain_plug = app().find_plugin<chain_plugin>();
                EOS_ASSERT(my->chain_plug, chain::missing_chain_plugin_exception, "");
                auto &chain = my->chain_plug->chain();
                my->chain_id.emplace(chain.get_chain_id());

                // my->accepted_block_connection.emplace(
                //         chain.accepted_block.connect([&](const chain::block_state_ptr &bs) {
                //             my->accepted_block(bs);
                //         }));

                // my->irreversible_block_connection.emplace(
                //         chain.irreversible_block.connect([&](const chain::block_state_ptr &bs) {
                //             my->applied_irreversible_block(bs);
                //         }));

                // my->accepted_transaction_connection.emplace(
                //         chain.accepted_transaction.connect([&](const chain::transaction_metadata_ptr &t) {
                //             my->accepted_transaction(t);
                //         }));

                my->applied_transaction_connection.emplace(
                    chain.applied_transaction.connect(
                        [&](std::tuple<const chain::transaction_trace_ptr &, const chain::signed_transaction &> t) {
                            my->applied_transaction(std::get<0>(t));
                        }
                    )
                );
                my->init();
            } else {
                wlog("eosio::kafka_plugin configured, but no --kafka-uri specified.");
                wlog("kafka_plugin disabled.");
            }

        }

        FC_LOG_AND_RETHROW()
    }

    void kafka_plugin::plugin_startup() {
        app().get_plugin<http_plugin>().add_api( api_description {
            {
                "/v1/kafka/set_log_filter",
                [&] (string, string body, url_response_callback cb) { return my->set_log_filter(move(body), move(cb)); }
            }
        });
    }

    void kafka_plugin::plugin_shutdown() {
        // my->accepted_block_connection.reset();
        // my->irreversible_block_connection.reset();
        // my->accepted_transaction_connection.reset();
        my->applied_transaction_connection.reset();
        my.reset();
    }

} // namespace eosio

#include <fc/reflect/reflect.hpp>
FC_REFLECT(eosio::kafka_plugin_impl::action_info, (global_sequence)(action_ordinal)(account)(receiver)(name)(authorization)(data))
FC_REFLECT(eosio::kafka_plugin_impl::transaction_info, (id)(block_number)(block_time)(receipt)(actions))
FC_REFLECT(eosio::kafka_plugin_impl::set_log_filter_params, (prev)(accounts))
