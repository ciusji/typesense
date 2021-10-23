#include "batched_indexer.h"
#include "core_api.h"
#include "thread_local_vars.h"

BatchedIndexer::BatchedIndexer(HttpServer* server, Store* store, const size_t num_threads):
                               server(server), store(store), num_threads(num_threads),
                               last_gc_run(std::chrono::high_resolution_clock::now()), quit(false) {
    queues.resize(num_threads);
    qmutuxes = new std::mutex[num_threads];
}

void BatchedIndexer::enqueue(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    // Called by the raft write thread: goal is to quickly send the request to a queue and move on
    // NOTE: it's ok to access `req` and `res` in this function without synchronization
    // because the read thread for *this* request is paused now and resumes only messaged at the end

    //LOG(INFO) << "BatchedIndexer::enqueue";
    uint32_t chunk_sequence = 0;

    {
        std::unique_lock lk(mutex);
        auto req_res_map_it = req_res_map.find(req->start_ts);

        if(req_res_map_it == req_res_map.end()) {
            // first chunk
            uint64_t batch_begin_ts = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();

            req_res_t req_res("", req, res, batch_begin_ts, 1, 0, false);
            req_res_map.emplace(req->start_ts, req_res);
        } else {
            chunk_sequence = req_res_map_it->second.num_chunks;
            req_res_map_it->second.num_chunks += 1;
        }
    }

    const std::string& req_key_prefix = get_req_prefix_key(req->start_ts);
    const std::string& request_chunk_key = req_key_prefix + StringUtils::serialize_uint32_t(chunk_sequence);

    //LOG(INFO) << "insert request_chunk_key: " << request_chunk_key;

    store->insert(request_chunk_key, req->to_json());
    req->body = "";

    bool is_old_serialized_request = (req->start_ts == 0);
    bool read_more_input = (req->_req != nullptr && req->_req->proceed_req);

    if(req->last_chunk_aggregate) {
        //LOG(INFO) << "Last chunk for req_id: " << req->start_ts;
        queued_writes += (chunk_sequence + 1);

        {
            const std::string& coll_name = get_collection_name(req);
            uint64_t queue_id = StringUtils::hash_wy(coll_name.c_str(), coll_name.size()) % num_threads;
            std::unique_lock lk1(qmutuxes[queue_id]);
            queues[queue_id].emplace_back(req->start_ts);

            std::unique_lock lk2(mutex);
            req_res_map[req->start_ts].is_complete = true;
        }

        // IMPORTANT: must not read `req` variables (except _req) henceforth to prevent data races with indexing thread

        if(is_old_serialized_request) {
            // Indicates a serialized request from a version that did not support batching (v0.21 and below).
            // We can only do serial writes as we cannot reliably distinguish one streaming request from another.
            // So, wait for `req_res_map` to be empty before proceeding
            while(true) {
                {
                    std::unique_lock lk(mutex);
                    if(req_res_map.empty()) {
                        break;
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds (10));
            }
        }
    }

    if(read_more_input) {
        // Tell the http library to read more input data
        deferred_req_res_t* req_res = new deferred_req_res_t(req, res, server, true);
        server->get_message_dispatcher()->send_message(HttpServer::REQUEST_PROCEED_MESSAGE, req_res);
    }
}

std::string BatchedIndexer::get_collection_name(const std::shared_ptr<http_req>& req) {
    std::string& coll_name = req->params["collection"];

    if(coll_name.empty()) {
        route_path* rpath;
        server->get_route(req->route_hash, &rpath);

        // ensure that collection creation is sent to the same queue as writes to that collection
        if(rpath->handler == post_create_collection) {
            nlohmann::json obj = nlohmann::json::parse(req->body, nullptr, false);

            if(obj != nlohmann::json::value_t::discarded && obj.is_object() &&
               obj.count("name") != 0 && obj["name"].is_string()) {
                coll_name = obj["name"];
            }
        }
    }

    return coll_name;
}

void BatchedIndexer::run() {
    LOG(INFO) << "Starting batch indexer with " << num_threads << " threads.";
    ThreadPool* thread_pool = new ThreadPool(num_threads);

    for(size_t i = 0; i < num_threads; i++) {
        std::deque<uint64_t>& queue = queues[i];
        std::mutex& queue_mutex = qmutuxes[i];

        thread_pool->enqueue([&queue, &queue_mutex, this, i]() {
            while(!quit) {
                std::unique_lock<std::mutex> qlk(queue_mutex);

                if(queue.empty()) {
                    qlk.unlock();
                } else {
                    uint64_t req_id = queue.front();
                    queue.pop_front();
                    qlk.unlock();

                    std::unique_lock mlk(mutex);
                    req_res_t& orig_req_res = req_res_map[req_id];
                    mlk.unlock();

                    // scan db for all logs associated with request
                    const std::string& req_key_prefix = get_req_prefix_key(req_id);

                    const std::string& req_key_start_prefix = get_req_prefix_key(req_id) +
                                                        StringUtils::serialize_uint32_t(orig_req_res.next_chunk_index);

                    rocksdb::Iterator* iter = store->scan(req_key_start_prefix);

                    // used to handle partial JSON documents caused by chunking
                    std::string& prev_body = orig_req_res.prev_req_body;

                    const std::shared_ptr<http_req>& orig_req = orig_req_res.req;
                    const std::shared_ptr<http_res>& orig_res = orig_req_res.res;
                    bool is_live_req = orig_res->is_alive;

                    route_path* found_rpath = nullptr;
                    bool route_found = server->get_route(orig_req->route_hash, &found_rpath);
                    bool async_res = false;

                    while(iter->Valid() && iter->key().starts_with(req_key_prefix)) {
                        std::shared_lock slk(pause_mutex); // used for snapshot
                        orig_req->body = prev_body;
                        orig_req->load_from_json(iter->value().ToString());

                        // update thread local for reference during a crash
                        write_log_index = orig_req->log_index;

                        //LOG(INFO) << "original request: " << orig_req_res.req << ", req: " << orig_req_res.req->req;

                        if(route_found) {
                            async_res = found_rpath->async_res;
                            found_rpath->handler(orig_req, orig_res);
                            prev_body = orig_req->body;
                        } else {
                            orig_res->set_404();
                        }

                        if(is_live_req && (!route_found ||!async_res)) {
                            // sync request get a response immediately
                            async_req_res_t* async_req_res = new async_req_res_t(orig_req, orig_res, true);
                            server->get_message_dispatcher()->send_message(HttpServer::STREAM_RESPONSE_MESSAGE, async_req_res);
                        }

                        if(!route_found) {
                            break;
                        }

                        queued_writes--;
                        orig_req_res.next_chunk_index++;
                        iter->Next();

                        if(quit) {
                            break;
                        }
                    }

                    delete iter;

                    //LOG(INFO) << "Erasing request data from disk and memory for request " << orig_req_res.req->start_ts;

                    // we can delete the buffered request content
                    store->delete_range(req_key_prefix, req_key_prefix + StringUtils::serialize_uint32_t(UINT32_MAX));

                    std::unique_lock lk(mutex);
                    req_res_map.erase(req_id);
                }

                std::this_thread::sleep_for(std::chrono::milliseconds (10));
            }
        });
    }

    while(!quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds (1000));

        //LOG(INFO) << "Batch indexer main thread";

        // do gc, if we are due for one
        uint64_t seconds_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::high_resolution_clock::now() - last_gc_run).count();

        if(seconds_elapsed > GC_INTERVAL_SECONDS) {

            std::unique_lock lk(mutex);
            LOG(INFO) << "Running GC for aborted requests, req map size: " << req_res_map.size();

            // iterate through all map entries and delete ones which are > GC_PRUNE_MAX_SECONDS
            for (auto it = req_res_map.cbegin(); it != req_res_map.cend();) {
                uint64_t seconds_since_batch_start = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count() - it->second.batch_begin_ts;

                //LOG(INFO) << "Seconds since batch start: " << seconds_since_batch_start;

                if(seconds_since_batch_start > GC_PRUNE_MAX_SECONDS) {
                    LOG(INFO) << "Deleting partial upload for req id " << it->second.req->start_ts;
                    const std::string& req_key_prefix = get_req_prefix_key(it->second.req->start_ts);
                    store->delete_range(req_key_prefix, req_key_prefix + StringUtils::serialize_uint32_t(UINT32_MAX));
                    it = req_res_map.erase(it);
                } else {
                    it++;
                }
            }

            last_gc_run = std::chrono::high_resolution_clock::now();
        }
    }

    LOG(INFO) << "Batched indexer threadpool shutdown...";
    thread_pool->shutdown();
    delete thread_pool;
}

std::string BatchedIndexer::get_req_prefix_key(uint64_t req_id) {
    const std::string& req_key_prefix =
            RAFT_REQ_LOG_PREFIX + StringUtils::serialize_uint64_t(req_id) + "_";

    return req_key_prefix;
}

BatchedIndexer::~BatchedIndexer() {
    delete [] qmutuxes;
}

void BatchedIndexer::stop() {
    quit = true;
}

int64_t BatchedIndexer::get_queued_writes() {
    return queued_writes;
}

void BatchedIndexer::serialize_state(nlohmann::json& state) {
    // requires external synchronization!
    state["queued_writes"] = queued_writes.load();
    state["req_res_map"] = nlohmann::json();

    size_t num_reqs_stored = 0;
    std::unique_lock lk(mutex);

    for(auto& kv: req_res_map) {
        std::string req_key = std::to_string(kv.first);
        state["req_res_map"].emplace(req_key, nlohmann::json());
        nlohmann::json& req_res = state["req_res_map"][req_key];
        req_res["batch_begin_ts"] = kv.second.batch_begin_ts;
        req_res["num_chunks"] = kv.second.num_chunks;
        req_res["next_chunk_index"] = kv.second.next_chunk_index;
        req_res["is_complete"] = kv.second.is_complete;
        req_res["req"] = kv.second.req->to_json();
        req_res["prev_req_body"] = kv.second.prev_req_body;
        num_reqs_stored++;

        //LOG(INFO) << "req_key: " << req_key << ", next_chunk_index: " << kv.second.next_chunk_index;
    }

    LOG(INFO) << "Serialized " << num_reqs_stored << " in-flight requests for snapshot.";
}

void BatchedIndexer::load_state(const nlohmann::json& state) {
    queued_writes = state["queued_writes"].get<int64_t>();

    size_t num_reqs_restored = 0;
    std::vector<uint64_t> queue_ids;

    for(auto& kv: state["req_res_map"].items()) {
        std::shared_ptr<http_req> req = std::make_shared<http_req>();
        req->load_from_json(kv.value()["req"].get<std::string>());

        std::shared_ptr<http_res> res = std::make_shared<http_res>(nullptr);
        req_res_t req_res(kv.value()["prev_req_body"].get<std::string>(), req, res,
                          kv.value()["batch_begin_ts"].get<uint64_t>(),
                          kv.value()["num_chunks"].get<uint32_t>(),
                          kv.value()["next_chunk_index"].get<uint32_t>(),
                          kv.value()["is_complete"].get<bool>());

        {
            std::unique_lock mlk(mutex);
            req_res_map.emplace(std::stoull(kv.key()), req_res);
        }

        // add only completed requests to their respective collection-based queues
        // the rest will be added by enqueue() when raft log is completely read

        if(req_res.is_complete) {
            LOG(INFO) << "req_res.next_chunk_index: " << req_res.next_chunk_index;
            const std::string& coll_name = get_collection_name(req);
            uint64_t queue_id = StringUtils::hash_wy(coll_name.c_str(), coll_name.size()) % num_threads;
            queue_ids.push_back(queue_id);
            std::unique_lock qlk(qmutuxes[queue_id]);
            queues[queue_id].emplace_back(req->start_ts);
        }

        num_reqs_restored++;
    }

    // need to sort on `start_ts` to preserve original order
    for(auto queue_id: queue_ids) {
        std::unique_lock lk(qmutuxes[queue_id]);
        std::sort(queues[queue_id].begin(), queues[queue_id].end());
    }

    LOG(INFO) << "Restored " << num_reqs_restored << " in-flight requests from snapshot.";
}

std::shared_mutex& BatchedIndexer::get_pause_mutex() {
    return pause_mutex;
}
