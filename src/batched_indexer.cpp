#include "batched_indexer.h"

BatchedIndexer::BatchedIndexer(HttpServer* server, Store* store, const size_t num_threads):
                               server(server), store(store), num_threads(num_threads),
                               last_gc_run(std::chrono::high_resolution_clock::now()), exit(false) {
    thread_pool = new ThreadPool(num_threads);
    queues.resize(num_threads);
    qmutuxes = new std::mutex[num_threads];
}

void BatchedIndexer::enqueue(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    const std::string& coll_name = req->params["collection"];
    uint64_t queue_id = StringUtils::hash_wy(coll_name.c_str(), coll_name.size()) % num_threads;

    uint32_t chunk_sequence = 0;

    {
        std::unique_lock lk(mutex);
        chunk_sequence = request_to_chunk[req->start_ts];
        request_to_chunk[req->start_ts] += 1;
    }

    const std::string& req_key_prefix = get_req_prefix_key(req->start_ts);
    const std::string& request_chunk_key = req_key_prefix + StringUtils::serialize_uint32_t(chunk_sequence);

    //LOG(INFO) << "req_id: " << req->start_ts << ", chunk_sequence: " << chunk_sequence;

    store->insert(request_chunk_key, req->serialize());
    queued_writes++;
    req->body = "";

    {
        std::unique_lock lk(mutex);
        auto req_res_map_it = req_res_map.find(req->start_ts);
        if(req_res_map_it == req_res_map.end()) {
            uint64_t batch_begin_ts = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();

            req_res_t req_res{req, res, batch_begin_ts};
            req_res_map[req->start_ts] = req_res;
        }
    }

    if(req->last_chunk_aggregate) {
        //LOG(INFO) << "Last chunk for req_id: " << req->start_ts;

        {
            std::unique_lock lk(qmutuxes[queue_id]);
            queues[queue_id].emplace_back(req->start_ts);
        }

        std::unique_lock lk(mutex);
        request_to_chunk.erase(req->start_ts);
    }

    if(req->_req != nullptr && req->_req->proceed_req) {
        deferred_req_res_t* req_res = new deferred_req_res_t(req, res, server, true);
        server->get_message_dispatcher()->send_message(HttpServer::REQUEST_PROCEED_MESSAGE, req_res);
    }
}

void BatchedIndexer::run() {
    LOG(INFO) << "Starting batch indexer with " << num_threads << " threads.";

    for(size_t i = 0; i < num_threads; i++) {
        std::deque<uint64_t>& queue = queues[i];
        std::mutex& queue_mutex = qmutuxes[i];

        thread_pool->enqueue([&queue, &queue_mutex, this, i]() {
            while(!exit) {
                std::unique_lock<std::mutex> qlk(queue_mutex);

                if(queue.empty()) {
                    qlk.unlock();
                } else {
                    uint64_t req_id = queue.front();
                    queue.pop_front();
                    qlk.unlock();

                    std::unique_lock mlk(mutex);
                    req_res_t orig_req_res = req_res_map[req_id];
                    mlk.unlock();

                    // scan db for all logs associated with request
                    const std::string& req_key_prefix = get_req_prefix_key(req_id);

                    rocksdb::Iterator* iter = store->scan(req_key_prefix);
                    std::string prev_body = "";  // used to handle partial JSON documents caused by chunking

                    while(iter->Valid() && iter->key().starts_with(req_key_prefix)) {
                        std::shared_ptr<http_req>& orig_req = orig_req_res.req;
                        auto _req = orig_req->_req;
                        orig_req->body = prev_body;
                        orig_req->deserialize(iter->value().ToString());
                        orig_req->_req = _req;

                        //LOG(INFO) << "original request: " << orig_req_res.req << ", _req: " << orig_req_res.req->_req;

                        route_path* found_rpath = nullptr;
                        bool route_found = server->get_route(orig_req->route_hash, &found_rpath);
                        bool async_res = false;

                        if(route_found) {
                            async_res = found_rpath->async_res;
                            found_rpath->handler(orig_req, orig_req_res.res);
                            prev_body = orig_req->body;
                        } else {
                            orig_req_res.res->set_404();
                            prev_body = "";
                        }

                        if(!async_res && orig_req_res.req->_req != nullptr) {
                            deferred_req_res_t* deferred_req_res = new deferred_req_res_t(orig_req_res.req,
                                                                                          orig_req_res.res,
                                                                                          server, true);
                            server->get_message_dispatcher()->send_message(HttpServer::STREAM_RESPONSE_MESSAGE,
                                                                           deferred_req_res);
                        }

                        queued_writes--;
                        iter->Next();
                    }

                    delete iter;

                    //LOG(INFO) << "Erasing request data from disk and memory for request " << req_res.req->start_ts;

                    // we can delete the buffered request content
                    store->delete_range(req_key_prefix, req_key_prefix + StringUtils::serialize_uint32_t(UINT32_MAX));

                    std::unique_lock lk(mutex);
                    req_res_map.erase(req_id);
                }

                std::this_thread::sleep_for(std::chrono::milliseconds (10));
            }
        });
    }

    while(!exit) {
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
                    request_to_chunk.erase(it->second.req->start_ts);
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
}

std::string BatchedIndexer::get_req_prefix_key(uint64_t req_id) {
    const std::string& req_key_prefix =
            RAFT_REQ_LOG_PREFIX + StringUtils::serialize_uint64_t(req_id) + "_";

    return req_key_prefix;
}

BatchedIndexer::~BatchedIndexer() {
    delete [] qmutuxes;
    delete thread_pool;
}

void BatchedIndexer::stop() {
    exit = true;
}

int64_t BatchedIndexer::get_queued_writes() {
    return queued_writes;
}
