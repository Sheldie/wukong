/*
 * Copyright (c) 2016 Shanghai Jiao Tong University.
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://ipads.se.sjtu.edu.cn/projects/wukong
 *
 */

#pragma once

#include <boost/unordered_set.hpp>
#include <boost/unordered_map.hpp>
#include <stdlib.h> //qsort

#include "config.hpp"
#include "type.hpp"
#include "coder.hpp"
#include "adaptor.hpp"
#include "dgraph.hpp"
#include "query.hpp"

#include "mymath.hpp"
#include "timer.hpp"

using namespace std;

// The map is used to colloect the replies of sub-queries in fork-join execution
class Reply_Map {
private:

    struct Item {
        int count;
        SPARQLQuery parent_request;
        SPARQLQuery merged_reply;
    };

    boost::unordered_map<int, Item> internal_item_map;

public:
    void put_parent_request(SPARQLQuery &r, int cnt) {
        Item data;
        data.count = cnt;
        data.parent_request = r;
        internal_item_map[r.id] = data;
    }

    void put_reply(SPARQLQuery &r) {
        int pid = r.pid;
        Item &data = internal_item_map[pid];

        SPARQLQuery::Result &data_result = data.merged_reply.result;
        SPARQLQuery::Result &r_result = r.result;
        data.count --;
        data_result.col_num = r_result.col_num;
        data_result.blind = r_result.blind;
        data_result.row_num += r_result.row_num;
        data_result.attr_col_num = r_result.attr_col_num;
        data_result.v2c_map = r_result.v2c_map;

        data_result.append_result(r_result);
    }

    bool is_ready(int pid) {
        return internal_item_map[pid].count == 0;
    }

    SPARQLQuery get_merged_reply(int pid) {
        SPARQLQuery r = internal_item_map[pid].parent_request;
        SPARQLQuery &merged_reply = internal_item_map[pid].merged_reply;

        r.result.col_num = merged_reply.result.col_num;
        r.result.blind = merged_reply.result.blind;
        r.result.row_num = merged_reply.result.row_num;
        r.result.attr_col_num = merged_reply.result.attr_col_num;
        r.result.v2c_map = merged_reply.result.v2c_map;

        r.result.result_table.swap(merged_reply.result.result_table);
        r.result.attr_res_table.swap(r.result.attr_res_table);
        internal_item_map.erase(pid);
        return r;
    }
};



typedef pair<int64_t, int64_t> v_pair;

int64_t hash_pair(const v_pair &x) {
    int64_t r = x.first;
    r = r << 32;
    r += x.second;
    return hash<int64_t>()(r);
}


// a vector of pointers of all local engines
class Engine;
std::vector<Engine *> engines;


class Engine {
private:
    class Message {
    public:
        int sid;
        int tid;
        Bundle bundle;

        Message(int sid, int tid, Bundle &bundle)
            : sid(sid), tid(tid), bundle(bundle) { }
    };

    pthread_spinlock_t recv_lock;
    std::vector<SPARQLQuery> msg_fast_path;

    Reply_Map rmap; // a map of replies for pending (fork-join) queries
    pthread_spinlock_t rmap_lock;

    vector<Message> pending_msgs;

    inline void sweep_msgs() {
        if (!pending_msgs.size()) return;

        cout << "[INFO]#" << tid << " " << pending_msgs.size() << " pending msgs on engine." << endl;
        for (vector<Message>::iterator it = pending_msgs.begin(); it != pending_msgs.end();)
            if (adaptor->send(it->sid, it->tid, it->bundle))
                it = pending_msgs.erase(it);
            else
                ++it;
    }

    bool send_request(Bundle &bundle, int dst_sid, int dst_tid) {
        if (adaptor->send(dst_sid, dst_tid, bundle))
            return true;

        // failed to send, then stash the msg to void deadlock
        pending_msgs.push_back(Message(dst_sid, dst_tid, bundle));
        return false;
    }


    void const_to_known(SPARQLQuery &req) { assert(false); } /// TODO

    void const_to_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        std::vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;

        // the query plan is wrong
        assert(result.get_col_num() == 0);

        uint64_t sz = 0;
        edge_t *res = graph->get_edges_global(tid, start, d, pid, &sz);
        for (uint64_t k = 0; k < sz; k++)
            updated_result_table.push_back(res[k].val);

        result.result_table.swap(updated_result_table);
        result.add_var2col(end, 0);
        result.set_col_num(1);
        req.step++;
    }

    // all of these means const attribute
    void const_to_unknown_attr(SPARQLQuery & req ) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t aid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        std::vector<attr_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;

        assert(d == OUT); // attribute always uses OUT
        int type = SID_t;

        attr_t v;
        graph->get_vertex_attr_global(tid, start, d, aid, v);
        updated_result_table.push_back(v);
        type = boost::apply_visitor(get_type, v);

        result.attr_res_table.swap(updated_result_table);
        result.add_var2col(end, 0, type);
        result.set_attr_col_num(1);
        req.step++;
    }

    void known_to_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        std::vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;

        // the query plan is wrong
        //assert(req.get_col_num() == req.var2col(end));

        updated_result_table.reserve(result.result_table.size());
        for (int i = 0; i < result.get_row_num(); i++) {
            sid_t prev_id = result.get_row_col(i, result.var2col(start));
            uint64_t sz = 0;
            edge_t *res = graph->get_edges_global(tid, prev_id, d, pid, &sz);
            for (uint64_t k = 0; k < sz; k++) {
                result.append_row_to(i, updated_result_table);
                updated_result_table.push_back(res[k].val);
            }
        }

        result.result_table.swap(updated_result_table);
        result.add_var2col(end, result.get_col_num());
        result.set_col_num(result.get_col_num() + 1);
        req.step++;
    }

    void known_to_unknown_attr(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        std::vector<attr_t> updated_attr_result_table;
        std::vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;

        // attribute always uses OUT
        assert(d == OUT);
        int type = SID_t;

        updated_attr_result_table.reserve(result.attr_res_table.size());
        for (int i = 0; i < result.get_row_num(); i++) {
            sid_t prev_id = result.get_row_col(i, result.var2col(start));
            attr_t v;
            bool has_value = graph->get_vertex_attr_global(tid, prev_id, d, pid, v);
            if (has_value) {
                result.append_row_to(i, updated_result_table);
                result.append_attr_row_to(i, updated_attr_result_table);
                updated_attr_result_table.push_back(v);
                type = boost::apply_visitor(get_type, v);
            }
        }

        result.attr_res_table.swap(updated_attr_result_table);
        result.result_table.swap(updated_result_table);
        result.add_var2col(end, result.get_attr_col_num(), type);
        result.set_attr_col_num(result.get_attr_col_num() + 1);
        req.step++;
    }

    void known_to_known(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        vector<sid_t> updated_result_table;
        vector<attr_t> updated_attr_res_table;
        SPARQLQuery::Result &result = req.result;

        for (int i = 0; i < result.get_row_num(); i++) {
            sid_t prev_id = result.get_row_col(i, result.var2col(start));
            uint64_t sz = 0;
            edge_t *res = graph->get_edges_global(tid, prev_id, d, pid, &sz);
            sid_t end2 = result.get_row_col(i, result.var2col(end));
            for (uint64_t k = 0; k < sz; k++) {
                if (res[k].val == end2) {
                    result.append_row_to(i, updated_result_table);
                    if (global_enable_vattr)
                        result.append_attr_row_to(i, updated_attr_res_table);
                    break;
                }
            }
        }

        result.result_table.swap(updated_result_table);
        if (global_enable_vattr)
            result.attr_res_table.swap(updated_attr_res_table);
        req.step++;
    }

    void known_to_const(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        vector<sid_t> updated_result_table;
        vector<attr_t> updated_attr_res_table;
        SPARQLQuery::Result &result = req.result;        

        for (int i = 0; i < result.get_row_num(); i++) {
            sid_t prev_id = result.get_row_col(i, result.var2col(start));
            uint64_t sz = 0;
            edge_t *res = graph->get_edges_global(tid, prev_id, d, pid, &sz);
            for (uint64_t k = 0; k < sz; k++) {
                if (res[k].val == end) {
                    result.append_row_to(i, updated_result_table);
                    if (global_enable_vattr)
                        result.append_attr_row_to(i, updated_attr_res_table);
                    break;
                }
            }
        }

        result.result_table.swap(updated_result_table);
        if (global_enable_vattr)
            result.attr_res_table.swap(updated_attr_res_table);
        req.step++;
    }

    void index_to_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t tpid = pattern.subject;
        ssid_t id01   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t var   = pattern.object;
        vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;                

        assert(id01 == PREDICATE_ID || id01 == TYPE_ID); // predicate or type index

        assert(result.get_col_num() == 0); // the query plan is wrong

        uint64_t sz = 0;
        edge_t *res = graph->get_index_edges_local(tid, tpid, d, &sz);
        int start = req.tid;
        for (uint64_t k = start; k < sz; k += global_mt_threshold)
            updated_result_table.push_back(res[k].val);

        result.result_table.swap(updated_result_table);
        result.set_col_num(1);
        result.add_var2col(var, 0);
        req.step++;
        req.local_var = -1;
    }

    // e.g., "<http://www.Department0.University0.edu> ?P ?X"
    void const_unknown_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;                

        // the query plan is wrong
        assert(result.get_col_num() == 0);

        uint64_t npids = 0;
        edge_t *pids = graph->get_edges_global(tid, start, d, PREDICATE_ID, &npids);

        // use a local buffer to store "known" predicates
        edge_t *tpids = (edge_t *)malloc(npids * sizeof(edge_t));
        memcpy((char *)tpids, (char *)pids, npids * sizeof(edge_t));

        for (uint64_t p = 0; p < npids; p++) {
            uint64_t sz = 0;
            edge_t *res = graph->get_edges_global(tid, start, d, tpids[p].val, &sz);
            for (uint64_t k = 0; k < sz; k++) {
                updated_result_table.push_back(tpids[p].val);
                updated_result_table.push_back(res[k].val);
            }
        }

        free(tpids);

        result.result_table.swap(updated_result_table);
        result.set_col_num(2);
        result.add_var2col(pid, 0);
        result.add_var2col(end, 1);
        req.step++;
    }

    // e.g., "<http://www.University0.edu> ub:subOrganizationOf ?D"
    //       "?D ?P ?X"
    void known_unknown_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;                

        for (int i = 0; i < result.get_row_num(); i++) {
            sid_t prev_id = result.get_row_col(i, result.var2col(start));
            uint64_t npids = 0;
            edge_t *pids = graph->get_edges_global(tid, prev_id, d, PREDICATE_ID, &npids);

            // use a local buffer to store "known" predicates
            edge_t *tpids = (edge_t *)malloc(npids * sizeof(edge_t));
            memcpy((char *)tpids, (char *)pids, npids * sizeof(edge_t));

            for (uint64_t p = 0; p < npids; p++) {
                uint64_t sz = 0;
                edge_t *res = graph->get_edges_global(tid, prev_id, d, tpids[p].val, &sz);
                for (uint64_t k = 0; k < sz; k++) {
                    result.append_row_to(i, updated_result_table);
                    updated_result_table.push_back(tpids[p].val);
                    updated_result_table.push_back(res[k].val);
                }
            }

            free(tpids);
        }

        result.result_table.swap(updated_result_table);
        result.set_col_num(result.get_col_num() + 2);
        result.add_var2col(pid, result.get_col_num());
        result.add_var2col(end, result.get_col_num() + 1);
        req.step++;
    }

    // FIXME: deadcode
    void known_unknown_const(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;                

        for (int i = 0; i < result.get_row_num(); i++) {
            sid_t prev_id = result.get_row_col(i, result.var2col(start));
            uint64_t npids = 0;
            edge_t *pids = graph->get_edges_global(tid, prev_id, d, PREDICATE_ID, &npids);

            // use a local buffer to store "known" predicates
            edge_t *tpids = (edge_t *)malloc(npids * sizeof(edge_t));
            memcpy((char *)tpids, (char *)pids, npids * sizeof(edge_t));

            for (uint64_t p = 0; p < npids; p++) {
                uint64_t sz = 0;
                edge_t *res = graph->get_edges_global(tid, prev_id, d, tpids[p].val, &sz);
                for (uint64_t k = 0; k < sz; k++) {
                    if (res[k].val == end) {
                        result.append_row_to(i, updated_result_table);
                        updated_result_table.push_back(tpids[p].val);
                        break;
                    }
                }
            }

            free(tpids);
        }

        result.add_var2col(pid, result.get_col_num());
        result.set_col_num(result.get_col_num() + 1);
        result.result_table.swap(updated_result_table);
        req.step++;
    }

    vector<SPARQLQuery> generate_sub_query(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;

        // generate sub requests for all servers
        vector<SPARQLQuery> sub_reqs(global_num_servers);
        for (int i = 0; i < global_num_servers; i++) {
            sub_reqs[i].pid = req.id;
            sub_reqs[i].pattern_group = req.pattern_group;
            sub_reqs[i].step = req.step;
            sub_reqs[i].corun_step = req.corun_step;
            sub_reqs[i].fetch_step = req.fetch_step;
            sub_reqs[i].local_var = start;

            sub_reqs[i].result.col_num = req.result.col_num;
            sub_reqs[i].result.blind = req.result.blind;
            sub_reqs[i].result.v2c_map  = req.result.v2c_map;
            sub_reqs[i].result.required_vars  = req.result.required_vars;
            sub_reqs[i].result.nvars  = req.result.nvars;
        }

        // group intermediate results to servers
        for (int i = 0; i < req.result.get_row_num(); i++) {
            int dst_sid = mymath::hash_mod(req.result.get_row_col(i, req.result.var2col(start)),
                                           global_num_servers);
            req.result.append_row_to(i, sub_reqs[dst_sid].result.result_table);
        }

        return sub_reqs;
    }

    // fork-join or in-place execution
    bool need_fork_join(SPARQLQuery &req) {
        // always need fork-join mode w/o RDMA
        if (!global_use_rdma) return true;

        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        return ((req.local_var != start)
                && (req.result.get_row_num() >= global_rdma_threshold));
    }

    void do_corun(SPARQLQuery &req) {
        SPARQLQuery::Result &req_result = req.result;
        int corun_step = req.corun_step;
        int fetch_step = req.fetch_step;

        // step.1 remove dup;
        uint64_t t0 = timer::get_usec();

        boost::unordered_set<sid_t> unique_set;
        ssid_t vid = req.get_pattern(corun_step).subject;
        assert(vid < 0);
        int col_idx = req_result.var2col(vid);
        for (int i = 0; i < req_result.get_row_num(); i++)
            unique_set.insert(req_result.get_row_col(i, col_idx));

        // step.2 generate cmd_chain for sub-reqs
        SPARQLQuery::PatternGroup subgroup;
        vector<int> pvars_map; // from new_id to col_idx of id

        boost::unordered_map<sid_t, sid_t> sub_pvars;

        auto lambda = [&](ssid_t id) -> ssid_t {
            if (id < 0) { // remap pattern variable
                if (sub_pvars.find(id) == sub_pvars.end()) {
                    sid_t new_id = - (sub_pvars.size() + 1); // starts from -1
                    sub_pvars[id] = new_id;
                    pvars_map.push_back(req_result.var2col(id));
                }
                return sub_pvars[id];
            } else {
                return id;
            }
        };

        for(int i = corun_step; i < fetch_step; i++) {
            SPARQLQuery::Pattern &pattern = req.get_pattern(i);
            ssid_t subject = lambda(pattern.subject);
            ssid_t predicate = lambda(pattern.predicate);
            dir_t direction = pattern.direction;
            ssid_t object = lambda(pattern.object);
            SPARQLQuery::Pattern newPattern(subject,predicate,direction,object);
            newPattern.pred_type = 0;
            subgroup.patterns.push_back(newPattern);
        }

        // step.3 make sub-req
        SPARQLQuery sub_req;
        SPARQLQuery::Result &sub_result = sub_req.result;

        // query
        sub_req.pattern_group = subgroup;
        sub_result.nvars = pvars_map.size();

        // result
        boost::unordered_set<sid_t>::iterator iter;
        for (iter = unique_set.begin(); iter != unique_set.end(); iter++)
            sub_result.result_table.push_back(*iter);
        sub_result.col_num = 1;

        //init var_map
        sub_result.add_var2col(sub_pvars[vid], 0);

        sub_result.blind = false; // must take back results
        uint64_t t1 = timer::get_usec(); // time to generate the sub-request

        // step.4 execute sub-req
        while (true) {
            execute_one_step(sub_req);
            if (sub_req.is_finished())
                break;
        }
        uint64_t t2 = timer::get_usec(); // time to run the sub-request

        uint64_t t3, t4;
        vector<sid_t> updated_result_table;

        if (sub_result.get_col_num() > 2) { // qsort
            mytuple::qsort_tuple(sub_result.get_col_num(), sub_result.result_table);

            t3 = timer::get_usec();
            vector<sid_t> tmp_vec;
            tmp_vec.resize(sub_result.get_col_num());
            for (int i = 0; i < req_result.get_row_num(); i++) {
                for (int c = 0; c < pvars_map.size(); c++)
                    tmp_vec[c] = req_result.get_row_col(i, pvars_map[c]);

                if (mytuple::binary_search_tuple(sub_result.get_col_num(),
                                                 sub_result.result_table, tmp_vec))
                    req_result.append_row_to(i, updated_result_table);
            }
            t4 = timer::get_usec();
        } else { // hash join
            boost::unordered_set<v_pair> remote_set;
            for (int i = 0; i < sub_result.get_row_num(); i++)
                remote_set.insert(v_pair(sub_result.get_row_col(i, 0),
                                         sub_result.get_row_col(i, 1)));

            t3 = timer::get_usec();
            vector<sid_t> tmp_vec;
            tmp_vec.resize(sub_result.get_col_num());
            for (int i = 0; i < req_result.get_row_num(); i++) {
                for (int c = 0; c < pvars_map.size(); c++)
                    tmp_vec[c] = req_result.get_row_col(i, pvars_map[c]);

                v_pair target = v_pair(tmp_vec[0], tmp_vec[1]);
                if (remote_set.find(target) != remote_set.end())
                    req_result.append_row_to(i, updated_result_table);
            }
            t4 = timer::get_usec();
        }

        // debug
        if (sid == 0 && tid == 0) {
            cout << "prepare " << (t1 - t0) << " us" << endl;
            cout << "execute sub-request " << (t2 - t1) << " us" << endl;
            cout << "sort " << (t3 - t2) << " us" << endl;
            cout << "lookup " << (t4 - t3) << " us" << endl;
        }

        req_result.result_table.swap(updated_result_table);
        req.step = fetch_step;
    }

    bool execute_one_step(SPARQLQuery &req) {
        if (req.is_finished())
            return false;

        if (req.step == 0 && req.start_from_index()) {
            index_to_unknown(req);
            return true;
        }

        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start     = pattern.subject;
        ssid_t predicate = pattern.predicate;
        dir_t direction  = pattern.direction;
        ssid_t end       = pattern.object;

        // triple pattern with unknown predicate/attribute
        if (predicate < 0) {
#ifdef VERSATILE
            switch (var_pair(req.result.variable_type(start),
                             req.result.variable_type(end))) {
            case var_pair(const_var, unknown_var):
                const_unknown_unknown(req);
                break;
            case var_pair(known_var, unknown_var):
                known_unknown_unknown(req);
                break;
            default:
                cout << "ERROR: unsupported triple pattern with unknown predicate "
                     << "(" << req.result.variable_type(start) << "|" << req.result.variable_type(end) << ")"
                     << endl;
                assert(false);
            }
            return true;
#else
            cout << "ERROR: unsupported variable at predicate." << endl;
            cout << "Please add definition VERSATILE in CMakeLists.txt." << endl;
            assert(false);
#endif
        }

        // triple pattern with attribute
        if (global_enable_vattr && req.get_pattern(req.step).pred_type > 0) {
            switch (var_pair(req.result.variable_type(start),
                             req.result.variable_type(end))) {
            case var_pair(const_var, unknown_var):
                const_to_unknown_attr(req);
                break;
            case var_pair(known_var, unknown_var):
                known_to_unknown_attr(req);
                break;
            default:
                cout << "ERROR: unsupported triple pattern with attribute "
                     << "(" << req.result.variable_type(start) << "|" << req.result.variable_type(end) << ")"
                     << endl;
                assert(false);
            }
            return true;
        }

        // triple pattern with known predicate
        switch (var_pair(req.result.variable_type(start),
                         req.result.variable_type(end))) {

        // start from const
        case var_pair(const_var, const_var):
            cout << "ERROR: unsupported triple pattern (from const to const)" << endl;
            assert(false);
        case var_pair(const_var, known_var):
            cout << "ERROR: unsupported triple pattern (from const to known)" << endl;
            assert(false);
        case var_pair(const_var, unknown_var):
            const_to_unknown(req);
            break;

        // start from known
        case var_pair(known_var, const_var):
            known_to_const(req);
            break;
        case var_pair(known_var, known_var):
            known_to_known(req);
            break;
        case var_pair(known_var, unknown_var):
            known_to_unknown(req);
            break;

        // start from unknown
        case var_pair(unknown_var, const_var):
        case var_pair(unknown_var, known_var):
        case var_pair(unknown_var, unknown_var):
            cout << "ERROR: unsupported triple pattern (from unknown)" << endl;
            assert(false);

        default:
            cout << "ERROR: unsupported triple pattern with known predicate "
                 << "(" << req.result.variable_type(start) << "|" << req.result.variable_type(end) << ")"
                 << endl;
            assert(false);
        }

        return true;
    }

    void execute_request(SPARQLQuery &r) {
        SPARQLQuery::Result &result = r.result;
        r.id = coder.get_and_inc_qid();
        while (true) {
            uint64_t t1 = timer::get_usec();
            execute_one_step(r);
            t1 = timer::get_usec() - t1;

            // co-run optimization
            if (!r.is_finished() && (r.step == r.corun_step))
                do_corun(r);

            if (r.is_finished()) {
                result.row_num = result.get_row_num();
                if (result.blind)
                    result.clear_data(); // avoid take back the results
                Bundle bundle(r);
                send_request(bundle, coder.sid_of(r.pid), coder.tid_of(r.pid));
                return;
            }

            if (need_fork_join(r)) {
                vector<SPARQLQuery> sub_reqs = generate_sub_query(r);
                rmap.put_parent_request(r, sub_reqs.size());
                for (int i = 0; i < sub_reqs.size(); i++) {
                    if (i != sid) {
                        Bundle bundle(sub_reqs[i]);
                        send_request(bundle, i, tid);
                    } else {
                        pthread_spin_lock(&recv_lock);
                        msg_fast_path.push_back(sub_reqs[i]);
                        pthread_spin_unlock(&recv_lock);
                    }
                }
                return;
            }
        }
        return;
    }

    void execute_reply(SPARQLQuery &r, Engine *engine) {
        pthread_spin_lock(&engine->rmap_lock);
        engine->rmap.put_reply(r);

        if (engine->rmap.is_ready(r.pid)) {
            SPARQLQuery reply = engine->rmap.get_merged_reply(r.pid);
            pthread_spin_unlock(&engine->rmap_lock);
            Bundle bundle(reply);
            send_request(bundle, coder.sid_of(reply.pid), coder.tid_of(reply.pid));
        } else {
            pthread_spin_unlock(&engine->rmap_lock);
        }
    }

    void execute_sparql_request(SPARQLQuery &r, Engine *engine) {
        if (r.is_request())
            execute_request(r);
        else
            execute_reply(r, engine);
    }

#if DYNAMIC_GSTORE
    void execute_load_data(RDFLoad &r) {
        r.load_ret = graph->dynamic_load_data(r.load_dname, r.check_dup);
        Bundle bundle(r);
        send_request(bundle, coder.sid_of(r.pid), coder.tid_of(r.pid));
    }
#endif

    void execute(Bundle &bundle, Engine *engine) {
        if (bundle.type == SPARQL_QUERY) {
            SPARQLQuery r = bundle.get_sparql_query();
            execute_sparql_request(r, engine);
        }
#if DYNAMIC_GSTORE
        else if (bundle.type == DYNAMIC_LOAD) {
            RDFLoad r = bundle.get_rdf_load();
            execute_load_data(r);
        }
#endif
    }

public:
    const static uint64_t TIMEOUT_THRESHOLD = 10000; // 10 msec

    int sid;    // server id
    int tid;    // thread id

    DGraph *graph;
    Adaptor *adaptor;

    Coder coder;

    uint64_t last_time; // busy or not (work-oblige)

    Engine(int sid, int tid, DGraph *graph, Adaptor *adaptor)
        : sid(sid), tid(tid), graph(graph), adaptor(adaptor),
          coder(sid, tid), last_time(0) {
        pthread_spin_init(&recv_lock, 0);
        pthread_spin_init(&rmap_lock, 0);
    }

    void run() {
        // NOTE: the 'tid' of engine is not start from 0,
        // which can not be used by engines[] directly
        int own_id = tid - global_num_proxies;
        // TODO: replace pair to ring
        int nbr_id = (global_num_engines - 1) - own_id;

        int send_wait_cnt = 0;
        while (true) {
            Bundle bundle;
            bool success;

            // fast path
            last_time = timer::get_usec();
            success = false;

            SPARQLQuery request;
            pthread_spin_lock(&recv_lock);
            if (msg_fast_path.size() > 0) {
                request = msg_fast_path.back();
                msg_fast_path.pop_back();
                success = true;
            }
            pthread_spin_unlock(&recv_lock);

            if (success) {
                execute_sparql_request(request, engines[own_id]);
                continue; // fast-path priority
            }

            // check and send pending messages
            sweep_msgs();

            // normal path
            last_time = timer::get_usec();

            // own queue
            if (adaptor->tryrecv(bundle))
                execute(bundle, engines[own_id]);

            // work-oblige is disabled
            if (!global_enable_workstealing) continue;

            // neighbor queue
            last_time = timer::get_usec();
            if (last_time < engines[nbr_id]->last_time + TIMEOUT_THRESHOLD)
                continue; // neighboring worker is self-sufficient

            if (engines[nbr_id]->adaptor->tryrecv(bundle))
                execute(bundle, engines[nbr_id]);
        }
    }

};
