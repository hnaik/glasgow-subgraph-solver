/* vim: set sw=4 sts=4 et foldmethod=syntax : */

#include "lackey.hh"

#include <fstream>
#include <mutex>

using std::endl;
using std::function;
using std::ifstream;
using std::mutex;
using std::ofstream;
using std::string;
using std::to_string;
using std::unique_lock;

DisobedientLackeyError::DisobedientLackeyError(const std::string & m) noexcept :
    _what(m)
{
}

auto DisobedientLackeyError::what() const throw () -> const char *
{
    return _what.c_str();
}

struct Lackey::Imp
{
    mutex external_solver_mutex;

    ofstream send_to;
    ifstream read_from;
    const InputGraph & pattern_graph;
    const InputGraph & target_graph;

    long number_of_checks = 0, number_of_propagations = 0, number_of_deletions = 0;
};

Lackey::Lackey(const string & send_to_name, const string & read_from_name,
        const InputGraph & pattern_graph, const InputGraph & target_graph) :
    _imp(new Imp{ {}, ofstream{ send_to_name }, ifstream{ read_from_name }, pattern_graph, target_graph })
{
    if ((! _imp->read_from) || (! _imp->send_to))
        throw DisobedientLackeyError{ "error setting up lackey communication using " + send_to_name + " and " + read_from_name };
}

Lackey::~Lackey()
{
    if (_imp->send_to) {
        _imp->send_to << "Q 0" << endl;
    }
}

auto Lackey::check_solution(
        const VertexToVertexMapping & m,
        bool partial,
        bool all_solutions,
        const function<auto (int, int) -> bool> & deletion) -> bool
{
    unique_lock<mutex> lock{ _imp->external_solver_mutex };

    string command;
    if (partial) {
        if (deletion) {
            ++_imp->number_of_propagations;
            command = "P";
        }
        else {
            ++_imp->number_of_checks;
            command = "C";
        }
    }
    else {
        ++_imp->number_of_checks;
        if (all_solutions)
            command = "A";
        else
            command = "F";
    }

    _imp->send_to << command << " " << m.size();
    for (auto & [ p, t ] : m)
        _imp->send_to << " " << _imp->pattern_graph.vertex_name(p) << " " << _imp->target_graph.vertex_name(t);
    _imp->send_to << endl;

    if (! _imp->send_to)
        throw DisobedientLackeyError{ "error giving lackey its orders" };

    string operation;
    if (! (_imp->read_from >> operation) || operation != command)
        throw DisobedientLackeyError{ "asked lackey to " + command + ", but it replied with '" + operation + "'" };

    bool result;
    string response;
    if (! (_imp->read_from >> response))
        throw DisobedientLackeyError{ "asked lackey to " + command + ", but it gave no T/F" };
    else if (response == "T")
        result = true;
    else if (response == "F")
        result = false;
    else
        throw DisobedientLackeyError{ "asked lackey to " + command + " but it replied with '" + operation + "' then '" + response + "'" };

    int n;
    if (! (_imp->read_from >> n))
        throw DisobedientLackeyError{ "lackey replied with length '" + to_string(n) + "' to " + command + " query" };

    if (command == "S") {
        for (int i = 0 ; i < n ; ++i) {
            string k, v;
            if (! (_imp->read_from >> k >> v))
                throw DisobedientLackeyError{ "lackey gave bad response pair " + to_string(i) + " to " + command + " query" };
        }
    }
    else if (command == "C" || command == "P") {
        for (int i = 0 ; i < n ; ++i) {
            string k, v;
            int m;
            if (! (_imp->read_from >> k >> m))
                throw DisobedientLackeyError{ "lackey gave bad response pair " + k + " " + to_string(m) + " to " + command + " query" };
            auto p = _imp->pattern_graph.vertex_from_name(k);

            for (int j = 0 ; j < m ; ++j) {
                if (! (_imp->read_from >> v))
                    throw DisobedientLackeyError{ "lackey gave bad response pair " + k + " " + to_string(m) + " to " + command + " query" };

                if (deletion) {
                    auto t = _imp->target_graph.vertex_from_name(v);
                    if (p && t)
                        if (deletion(*p, *t))
                            ++_imp->number_of_deletions;
                }
            }
        }
    }

    return result;
}

auto Lackey::number_of_checks() const -> long
{
    return _imp->number_of_checks;
}

auto Lackey::number_of_propagations() const -> long
{
    return _imp->number_of_propagations;

}

auto Lackey::number_of_deletions() const -> long
{
    return _imp->number_of_deletions;
}

