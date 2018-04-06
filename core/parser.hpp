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

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <sstream>
#include <assert.h>
#include <boost/unordered_map.hpp>
#include <boost/algorithm/string.hpp>

#include "query.hpp"
#include "type.hpp"
#include "string_server.hpp"

#include "SPARQLParser.hpp"

using namespace std;

// Read a stream into a string
static string read_input(istream &in) {
    string result;
    while (true) {
        string s;
        getline(in, s);
        result += s;
        if (!in.good())
            break;
        result += '\n';
    }

    return result;
}

/**
 * Q := SELECT RD WHERE GP
 *
 * The types of tokens (supported)
 * 0. SPARQL's Prefix e.g., PREFIX rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#>
 * 1. SPARQL's Keyword (incl. SELECT, WHERE)
 *
 * 2. pattern's constant e.g., <http://www.Department0.University0.edu>
 * 3. pattern's variable e.g., ?X
 * 4. pattern's random-constant e.g., %ub:GraduateCourse (extended by Wukong in batch-mode)
 *
 */
class Parser {
private:
    // place holder of pattern type (a special group of objects)
    const static ssid_t PTYPE_PH = std::numeric_limits<ssid_t>::min() + 1;
    const static ssid_t DUMMY_ID = std::numeric_limits<ssid_t>::min();
    const static ssid_t PREDICATE_ID = 0;

    // str2ID mapping for pattern constants (e.g., <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> 1)
    String_Server *str_server;

    ssid_t encode(const SPARQLParser::Element& element) {//const
        switch (element.type) {
        case SPARQLParser::Element::Variable:
            return element.id;
        case SPARQLParser::Element::Literal:
        {
            string str = "\"" + element.value + "\"";
            if (!str_server->exist(str)) {
                logstream(LOG_ERROR) << "Unknown Literal: " + str << LOG_endl;
                return DUMMY_ID;
            }
            return str_server->str2id[str];
        }
        case SPARQLParser::Element::IRI:
        {
            string strIRI = "<" + element.value + ">" ;
            if (!str_server->exist(strIRI)) {
                logstream(LOG_ERROR) << "Unknown IRI: " + strIRI << LOG_endl;
                return DUMMY_ID;
            }
            return str_server->str2id[strIRI];
        }
        case SPARQLParser::Element::Template:
            return PTYPE_PH;
        case SPARQLParser::Element::Predicate:
            return PREDICATE_ID;
        default:
            return DUMMY_ID;
        }
        return DUMMY_ID;
    }

    void transfer_filter(SPARQLParser::Filter &src, SPARQLQuery::Filter &dest) {
        dest.type = (SPARQLQuery::Filter::Type)src.type;
        dest.value = src.value;
        dest.valueArg = src.valueArg;
        if (src.arg1 != NULL) {
            dest.arg1 = new SPARQLQuery::Filter();
            transfer_filter(*src.arg1, *dest.arg1);
        }
        if (src.arg2 != NULL) {
            dest.arg2 = new SPARQLQuery::Filter();
            transfer_filter(*src.arg2, *dest.arg2);
        }
        if (src.arg3 != NULL) {
            dest.arg3 = new SPARQLQuery::Filter();
            transfer_filter(*src.arg3, *dest.arg3);
        }
    }

    void transfer_patterns(SPARQLParser::PatternGroup &src, SPARQLQuery::PatternGroup &dest) {
        // patterns
        for (auto const &src_p : src.patterns) {
            SPARQLQuery::Pattern pattern(encode(src_p.subject),
                                         encode(src_p.predicate),
                                         src_p.direction,
                                         encode(src_p.object));
            int type =  str_server->pid2type[encode(src_p.predicate)];
            if (type > 0 && (!global_enable_vattr)) {
                logstream(LOG_ERROR) << "Need to change config to enable vertex_attr " << LOG_endl;
                assert(false);
            }
            pattern.pred_type = str_server->pid2type[encode(src_p.predicate)];
            dest.patterns.push_back(pattern);
        }
        // filters
        if (src.filters.size() > 0) {
            for (int i = 0; i < src.filters.size(); i++) {
                dest.filters.push_back(SPARQLQuery::Filter());
                transfer_filter(src.filters[i], dest.filters.back());
            }
        }
        // unions
        if (src.unions.size() > 0) {
            int i = 0;
            for (auto &union_group : src.unions) {
                dest.unions.push_back(SPARQLQuery::PatternGroup());
                transfer_patterns(union_group, dest.unions.back());
                dest.unions[i].patterns.insert(dest.unions[i].patterns.end(),
                                               dest.patterns.begin(), dest.patterns.end());
                i++;
            }
            dest.patterns.clear();
        }
        // optional
        if (src.optional.size() > 0) {
            for (auto &optional_group : src.optional) {
                dest.optional.push_back(SPARQLQuery::PatternGroup());
                transfer_patterns(optional_group, dest.optional.back());
            }
        }
        // other parts in PatternGroup
    }

    void transfer(const SPARQLParser &parser, SPARQLQuery &r) {
        SPARQLParser::PatternGroup group = parser.getPatterns();
        transfer_patterns(group, r.pattern_group);

        // init the var_map
        r.result.nvars = parser.getVariableCount();

        // required vars
        for (SPARQLParser::projection_iterator iter = parser.projectionBegin();
                iter != parser.projectionEnd();
                iter ++)
            r.result.required_vars.push_back(*iter);

        // optional
        if (r.pattern_group.optional.size() > 0)
            r.optional_dispatched = false;

        // orders
        for (SPARQLParser::order_iterator iter = parser.orderBegin();
                iter != parser.orderEnd();
                iter ++)
            r.orders.push_back(SPARQLQuery::Order((*iter).id, (*iter).descending));

        // limit and offset
        r.limit = parser.getLimit();
        r.offset = parser.getOffset();

        // distinct
        if ((parser.getProjectionModifier() == SPARQLParser::ProjectionModifier::Modifier_Distinct)
                || (parser.getProjectionModifier() == SPARQLParser::ProjectionModifier::Modifier_Reduced))
            r.distinct = true;

        // corun
        if (!global_use_rdma) {
            // TODO: corun optimization is not supported w/o RDMA
            logstream(LOG_WARNING) << "RDMA is not enabled, skip corun optimization!" << LOG_endl;
        } else {
            r.corun_step = parser.getCorunStep();
            r.fetch_step = parser.getFetchStep();
        }
    }

    ssid_t _H_push(const SPARQLParser::Element &element, request_template &r, int pos) {
        ssid_t id = encode(element);
        if (id == PTYPE_PH) {
            string strIRI = "<" + element.value + ">";
            r.ptypes_str.push_back(strIRI);
            r.ptypes_pos.push_back(pos);
        }
        return id;
    }


    void template_transfer(const SPARQLParser &parser, request_template &r) {
        SPARQLParser::PatternGroup group = parser.getPatterns();
        int pos = 0;
        for (std::vector<SPARQLParser::Pattern>::const_iterator iter = group.patterns.begin(),
                limit = group.patterns.end(); iter != limit; ++iter) {
            ssid_t subject = _H_push(iter->subject, r, pos++);
            ssid_t predicate = encode(iter->predicate); pos++;
            ssid_t direction = (dir_t)OUT; pos++;
            ssid_t object = _H_push(iter->object, r, pos++);
            SPARQLQuery::Pattern pattern(subject, predicate, direction, object);
            int type =  str_server->pid2type[encode(iter->predicate)];
            if (type > 0 && (!global_enable_vattr)) {
                logstream(LOG_ERROR) << "Need to change config to enable vertex_attr " << LOG_endl;
                assert(false);
            }
            pattern.pred_type = type;
            r.pattern_group.patterns.push_back(pattern);
        }

        // set the number of variables in triple patterns
        r.nvars = parser.getVariableCount();
    }

public:
    // the stat of query parsing
    std::string strerror;

    Parser(String_Server *_ss): str_server(_ss) {}

    /* Used in single-mode */
    bool parse(istream &is, SPARQLQuery &r) {
        // clear intermediate states of parser
        string query = read_input(is);
        SPARQLLexer lexer(query);
        SPARQLParser parser(lexer);
        try {
            parser.parse(); //e.g., sparql -f query/lubm_q1
            transfer(parser, r);
        } catch (const SPARQLParser::ParserException &e) {
            logstream(LOG_ERROR) << "failed to parse a SPARQL query: " << e.message << LOG_endl;
            return false;
        }

        // check if using custom grammar when planner is on
        if (parser.isUsingCustomGrammar() && global_enable_planner) {
            logstream(LOG_ERROR)  << "unsupported custom grammar in SPARQL planner!" << LOG_endl;
            return false;
        }

        logstream(LOG_INFO) << "parsing a query is done." << LOG_endl;
        return true;
    }

    /* Used in batch-mode */
    bool parse_template(istream &is, request_template &r) {
        string query = read_input(is);
        SPARQLLexer lexer(query);
        SPARQLParser parser(lexer);
        try {
            parser.parse();
            template_transfer(parser, r);
        } catch (const SPARQLParser::ParserException &e) {
            logstream(LOG_ERROR) << "failed to parse a SPARQL query: " << e.message << LOG_endl;
            return false;
        }
        return true;
    }
};
