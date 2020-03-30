/* vim: set sw=4 sts=4 et foldmethod=syntax : */

#include "clique.hh"
#include "watches.hh"
#include "svo_bitset.hh"
#include "proof.hh"
#include "configuration.hh"

#include <algorithm>
#include <list>
#include <numeric>
#include <random>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using std::find;
using std::iota;
using std::is_same;
using std::list;
using std::make_tuple;
using std::mt19937;
using std::move;
using std::pair;
using std::reverse;
using std::sort;
using std::swap;
using std::to_string;
using std::vector;

namespace
{
    enum class SearchResult
    {
        Aborted,
        Restart,
        Complete,
        DecidedTrue
    };

    struct Incumbent
    {
        unsigned value = 0;
        vector<int> c;

        auto update(const vector<int> & new_c, unsigned long long & find_nodes, unsigned long long & prove_nodes) -> void
        {
            if (new_c.size() > value) {
                find_nodes += prove_nodes;
                prove_nodes = 0;
                value = new_c.size();
                c = new_c;
            }
        }
    };

    template <typename EntryType_>
    struct FlatWatchTable
    {
        vector<EntryType_> data;

        EntryType_ & operator[] (int x)
        {
            return data[x];
        }
    };

    template <typename BitSetType_>
    struct CliqueRunner
    {
        const CliqueParams & params;
        Incumbent incumbent;

        int size;
        vector<BitSetType_> adj;
        vector<int> order, invorder;

        Watches<int, FlatWatchTable> watches;

        mt19937 global_rand;

        int * space;

        CliqueRunner(const InputGraph & g, const CliqueParams & p) :
            params(p),
            size(g.size()),
            adj(g.size(), BitSetType_{ unsigned(size), 0 }),
            order(size),
            invorder(size),
            space(nullptr)
        {
            space = new int[size * (size + 1) * 2];

            if (params.restarts_schedule->might_restart())
                watches.table.data.resize(g.size());

            // populate our order with every vertex initially
            iota(order.begin(), order.end(), 0);

            // pre-calculate degrees
            vector<int> degrees;
            degrees.resize(size);
            for (auto e = g.begin_edges(), e_end = g.end_edges() ; e != e_end ; ++e)
                ++degrees[e->first.first];

            // sort on degree
            if (! params.input_order)
                sort(order.begin(), order.end(),
                        [&] (int a, int b) { return true ^ (degrees[a] < degrees[b] || (degrees[a] == degrees[b] && a > b)); });

            for (unsigned i = 0 ; i < order.size() ; ++i)
                invorder[order[i]] = i;

            for (auto e = g.begin_edges(), e_end = g.end_edges() ; e != e_end ; ++e)
                adj[invorder[e->first.first]].set(invorder[e->first.second]);
        }

        ~CliqueRunner()
        {
            delete[] space;
        }

        auto colour_class_order(
                const BitSetType_ & p,
                int * p_order,
                int * p_bounds,
                int & p_end) -> void
        {
            BitSetType_ p_left = p;      // not coloured yet
            unsigned colour = 0;         // current colour
            p_end = 0;

            // while we've things left to colour
            while (p_left.any()) {
                // next colour
                ++colour;
                // things that can still be given this colour
                BitSetType_ q = p_left;

                // while we can still give something this colour
                while (q.any()) {
                    // first thing we can colour
                    int v = q.find_first();
                    p_left.reset(v);
                    q.reset(v);

                    // can't give anything adjacent to this the same colour
                    q.intersect_with_complement(adj[v]);

                    // record in result
                    p_bounds[p_end] = colour;
                    p_order[p_end] = v;
                    ++p_end;
                }
            }
        }

        auto colour_class_order_2df(
                const BitSetType_ & p,
                int * p_order,
                int * p_bounds,
                int * defer,
                int & p_end) -> void
        {
            BitSetType_ p_left = p;      // not coloured yet
            unsigned colour = 0;         // current colour
            p_end = 0;

            unsigned d = 0;             // number deferred

            // while we've things left to colour
            while (p_left.any()) {
                // next colour
                ++colour;
                // things that can still be given this colour
                BitSetType_ q = p_left;

                // while we can still give something this colour
                unsigned number_with_this_colour = 0;
                while (q.any()) {
                    // first thing we can colour
                    int v = q.find_first();
                    p_left.reset(v);
                    q.reset(v);

                    // can't give anything adjacent to this the same colour
                    q.intersect_with_complement(adj[v]);

                    // record in result
                    p_bounds[p_end] = colour;
                    p_order[p_end] = v;
                    ++p_end;
                    ++number_with_this_colour;
                }

                if (1 == number_with_this_colour) {
                    --p_end;
                    --colour;
                    defer[d++] = p_order[p_end];
                }
            }

            // handle deferred singletons
            for (unsigned n = 0 ; n < d ; ++n) {
                ++colour;
                p_order[p_end] = defer[n];
                p_bounds[p_end] = colour;
                ++p_end;
            }
        }

        auto colour_class_order_sorted(
                const BitSetType_ & p,
                int * p_order,
                int * p_bounds,
                int & p_end) -> void
        {
            BitSetType_ p_left = p;      // not coloured yet
            unsigned colour = 0;         // current colour
            p_end = 0;

            std::vector<int> p_order_prelim(size);
            std::vector<int> colour_sizes(size);
            std::vector<int> colour_start(size);
            std::vector<int> sorted_order(size);

            // while we've things left to colour
            while (p_left.any()) {
                colour_start[colour] = p_end;
                colour_sizes[colour] = 0;

                // next colour
                ++colour;
                // things that can still be given this colour
                BitSetType_ q = p_left;

                // while we can still give something this colour
                while (q.any()) {
                    // first thing we can colour
                    int v = q.find_first();
                    p_left.reset(v);
                    q.reset(v);

                    // can't give anything adjacent to this the same colour
                    q.intersect_with_complement(adj[v]);

                    // record in result
                    p_order_prelim[p_end] = v;
                    ++p_end;
                    ++colour_sizes[colour - 1];
                }
            }

            // sort
            iota(sorted_order.begin(), sorted_order.begin() + colour, 0);
            sort(sorted_order.begin(), sorted_order.begin() + colour, [&] (int a, int b) {
                    return make_tuple(colour_sizes[b], a) < make_tuple(colour_sizes[a], b);
                    });

            // copy out
            int p_end2 = 0;
            for (unsigned c = 0 ; c < colour ; ++c) {
                for (int v = colour_start[sorted_order[c]] ; v < colour_start[sorted_order[c]] + colour_sizes[sorted_order[c]] ; ++v) {
                    p_bounds[p_end2] = c + 1;
                    p_order[p_end2] = p_order_prelim[v];
                    ++p_end2;
                }
            }
        }

        auto post_nogood(
                const vector<int> & c)
        {
            Nogood<int> nogood;
            nogood.literals.assign(c.begin(), c.end());
            watches.post_nogood(move(nogood));
        }

        auto unpermute(
                const vector<int> & v) -> vector<int>
        {
            vector<int> result;
            for (auto & w : v)
                result.push_back(order[w]);
            return result;
        }

        auto unpermute(
                const BitSetType_ & v) -> vector<int>
        {
            vector<int> result;

            auto w = v;
            while (w.any()) {
                auto p = w.find_first();
                w.reset(p);
                result.push_back(order[p]);
            }

            return result;
        }

        auto unpermute_and_finish(
                vector<int> & v) -> vector<pair<int, bool> >
        {
            vector<pair<int, bool> > result;
            for (auto & w : v)
                result.emplace_back(order[w], true);
            for (int w = 0 ; w < size ; ++w)
                if (result.end() == find_if(result.begin(), result.end(), [&] (auto & x) { return x.first == w; }))
                    result.emplace_back(w, false);
            return result;
        }

        auto expand(
                int depth,
                unsigned long long & nodes,
                unsigned long long & find_nodes,
                unsigned long long & prove_nodes,
                vector<int> & c,
                BitSetType_ & p,
                int spacepos) -> SearchResult
        {
            ++nodes;
            ++prove_nodes;

            if (params.proof)
                params.proof->expanding(depth, unpermute(c), unpermute(p));

            // initial colouring
            int * p_order = &space[spacepos];
            int * p_bounds = &space[spacepos + size];

            int p_end = 0;
            switch (params.colour_class_order) {
                case ColourClassOrder::ColourOrder:     colour_class_order(p, p_order, p_bounds, p_end); break;
                case ColourClassOrder::SingletonsFirst: colour_class_order_2df(p, p_order, p_bounds, &space[spacepos + 2 * size], p_end); break;
                case ColourClassOrder::Sorted:          colour_class_order_sorted(p, p_order, p_bounds, p_end); break;
            }

            // for each v in p... (v comes later)
            for (int n = p_end - 1 ; n >= 0 ; --n) {
                // bound, timeout or early exit?
                if (params.timeout->should_abort())
                    return SearchResult::Aborted;

                if (c.size() + p_bounds[n] <= incumbent.value) {
                    if (params.proof) {
                        vector<vector<int> > colour_classes;
                        for (int v = 0 ; v <= n ; ++v) {
                            if (0 == v || p_bounds[v - 1] != p_bounds[v])
                                colour_classes.emplace_back();
                            colour_classes.back().push_back(order[p_order[v]]);
                        }
                        params.proof->colour_bound(colour_classes);
                    }
                    break;
                }

                // if we've used k colours to colour k vertices, it's a clique
                if (p_bounds[n] == n + 1) {
                    auto c_save = c;
                    for ( ; n >= 0 ; --n)
                        c.push_back(p_order[n]);
                    incumbent.update(c, find_nodes, prove_nodes);

                    if (params.proof && ! params.decide) {
                        params.proof->start_level(0);
                        params.proof->new_incumbent(unpermute_and_finish(c));
                        params.proof->start_level(depth + 1);
                    }

                    if (params.decide && incumbent.value >= *params.decide) {
                        if (params.proof)
                            params.proof->post_solution(unpermute(c));

                        return SearchResult::DecidedTrue;
                    }

                    c = move(c_save);

                    break;
                }

                auto v = p_order[n];

                // consider taking v
                c.push_back(v);

                if (params.decide) {
                    incumbent.update(c, find_nodes, prove_nodes);
                    if (incumbent.value >= *params.decide) {
                        if (params.proof)
                            params.proof->post_solution(unpermute(c));

                        return SearchResult::DecidedTrue;
                    }
                }

                // filter p to contain vertices adjacent to v
                BitSetType_ new_p = p;
                new_p &= adj[v];

                if (params.restarts_schedule->might_restart())
                    watches.propagate(v,
                            [&] (int literal) { return c.end() == find(c.begin(), c.end(), literal); },
                            [&] (int literal) { new_p.reset(literal); }
                            );

                if (params.proof)
                    params.proof->start_level(depth + 1);

                if (new_p.any()) {
                    switch (expand(depth + 1, nodes, find_nodes, prove_nodes, c, new_p, spacepos + 2 * size)) {
                        case SearchResult::Aborted:
                            return SearchResult::Aborted;

                        case SearchResult::DecidedTrue:
                            return SearchResult::DecidedTrue;

                        case SearchResult::Complete:
                            break;

                        case SearchResult::Restart:
                            // restore assignments before posting nogoods, it's easier
                            c.pop_back();

                            // post nogoods for everything we've done so far
                            for (int m = p_end - 1 ; m > n ; --m) {
                                c.push_back(p_order[m]);
                                post_nogood(c);
                                c.pop_back();
                            }

                            return SearchResult::Restart;
                    }
                }
                else {
                    incumbent.update(c, find_nodes, prove_nodes);
                    if (params.proof && ! params.decide) {
                        params.proof->start_level(0);
                        params.proof->new_incumbent(unpermute_and_finish(c));
                        params.proof->start_level(depth + 1);
                    }
                }

                if (params.proof) {
                    params.proof->start_level(depth);
                    params.proof->backtrack_from_binary_variables(unpermute(c));
                    params.proof->forget_level(depth + 1);
                }

                // now consider not taking v
                c.pop_back();
                p.reset(v);
            }

            if (params.proof)
                params.proof->unexpanding(depth, unpermute(c));

            params.restarts_schedule->did_a_backtrack();
            if (params.restarts_schedule->should_restart()) {
                post_nogood(c);
                return SearchResult::Restart;
            }
            else
                return SearchResult::Complete;
        }

        auto run() -> CliqueResult
        {
            CliqueResult result;

            if (params.decide)
                incumbent.value = *params.decide - 1;

            // do the search
            bool done = false;
            unsigned number_of_restarts = 0;

            BitSetType_ p{ unsigned(size), 0 };
            for (int i = 0 ; i < size ; ++i)
                p.set(i);

            while (! done) {
                ++number_of_restarts;

                // start watching new nogoods
                done = watches.apply_new_nogoods(
                        [&] (int literal) { p.reset(literal); }
                        );

                if (done)
                    break;

                watches.clear_new_nogoods();

                auto new_p = p;
                vector<int> c;
                switch (expand(0, result.nodes, result.find_nodes, result.prove_nodes, c, new_p, 0)) {
                    case SearchResult::Complete:
                        done = true;
                        break;

                    case SearchResult::DecidedTrue:
                        done = true;
                        break;

                    case SearchResult::Aborted:
                        done = true;
                        break;

                    case SearchResult::Restart:
                        break;
                }

                params.restarts_schedule->did_a_restart();
            }

            if (params.restarts_schedule->might_restart())
                result.extra_stats.emplace_back("restarts = " + to_string(number_of_restarts));

            if (params.proof && params.decide && incumbent.c.empty())
                params.proof->finish_unsat_proof();
            else if (params.proof && ! params.decide)
                params.proof->finish_unsat_proof();

            result.clique.clear();
            for (auto & v : incumbent.c)
                result.clique.insert(order[v]);

            return result;
        }
    };
}

auto solve_clique_problem(const InputGraph & graph, const CliqueParams & params) -> CliqueResult
{
    if (params.proof) {
        if (! params.proof->has_clique_model()) {
            for (int q = 0 ; q < graph.size() ; ++q)
                params.proof->create_binary_variable(q, [&] (int v) { return graph.vertex_name(v); });

            params.proof->create_objective(graph.size(), params.decide);

            for (int p = 0 ; p < graph.size() ; ++p)
                for (int q = 0 ; q < p ; ++q)
                    if (! graph.adjacent(p, q))
                        params.proof->create_non_edge_constraint(p, q);

            params.proof->finalise_model();
        }
    }

    CliqueRunner<SVOBitset> runner{ graph, params };
    return runner.run();
}

