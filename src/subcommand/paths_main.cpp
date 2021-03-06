/** \file paths_main.cpp
 *
 * Defines the "vg paths" subcommand, which reads paths in the graph.
 */


#include <omp.h>
#include <unistd.h>
#include <getopt.h>

#include <iostream>

#include "subcommand.hpp"

#include "../vg.hpp"

using namespace std;
using namespace vg;
using namespace vg::subcommand;

void help_paths(char** argv) {
    cerr << "usage: " << argv[0] << " paths [options] <graph.vg>" << endl
        << "options:" << endl
        << "  inspection:" << endl
        << "    -x, --extract         return (as GAM alignments) the stored paths in the graph" << endl
        << "    -L, --list            return (as a list of names, one per line) the path names" << endl
        << "  generation:" << endl
        << "    -n, --node ID         starting at node with ID" << endl
        << "    -l, --max-length N    generate paths of at most length N" << endl
        << "    -e, --edge-max N      only consider paths which make edge choices at this many points" << endl
        << "    -s, --as-seqs         write each path as a sequence" << endl
        << "    -p, --path-only       only write kpaths from the graph if they traverse embedded paths" << endl;
}

int main_paths(int argc, char** argv) {

    if (argc == 2) {
        help_paths(argv);
        return 1;
    }

    int max_length = 0;
    int edge_max = 0;
    int64_t node_id = 0;
    bool as_seqs = false;
    bool extract = false;
    bool list_paths = false;
    bool path_only = false;

    int c;
    optind = 2; // force optind past command positional argument
    while (true) {
        static struct option long_options[] =

        {
            {"extract", no_argument, 0, 'x'},
            {"list", no_argument, 0, 'L'},
            {"node", required_argument, 0, 'n'},
            {"max-length", required_argument, 0, 'l'},
            {"edge-max", required_argument, 0, 'e'},
            {"as-seqs", no_argument, 0, 's'},
            {"path-only", no_argument, 0, 'p'},
            {0, 0, 0, 0}
        };

        int option_index = 0;
        c = getopt_long (argc, argv, "n:l:hse:xLp",
                long_options, &option_index);

        // Detect the end of the options.
        if (c == -1)
            break;

        switch (c)
        {

            case 'x':
                extract = true;
                break;
                
            case 'L':
                list_paths = true;
                break;

            case 'n':
                node_id = atoll(optarg);
                break;

            case 'l':
                max_length = atoi(optarg);
                break;

            case 'e':
                edge_max = atoi(optarg);
                break;

            case 's':
                as_seqs = true;
                break;

            case 'p':
                path_only = true;
                break;

            case 'h':
            case '?':
                help_paths(argv);
                exit(1);
                break;

            default:
                abort ();
        }
    }

    if (edge_max == 0) edge_max = max_length + 1;

    VG* graph;
    get_input_file(optind, argc, argv, [&](istream& in) {
        graph = new VG(in);
    });

    if (extract) {
        vector<Alignment> alns = graph->paths_as_alignments();
        write_alignments(cout, alns);
        delete graph;
        return 0;
    }
    
    if (list_paths) {
        graph->paths.for_each_name([&](const string& name) {
            cout << name << endl;
        });
        delete graph;
        return 0;
    }

    if (max_length == 0) {
        cerr << "error:[vg paths] a --max-length is required when generating paths" << endl;
    }

    function<void(size_t,Path&)> paths_to_seqs = [graph](size_t mapping_index, Path& p) {
        string seq = graph->path_sequence(p);
#pragma omp critical(cout)
        cout << seq << endl;
    };

    function<void(size_t,Path&)> paths_to_json = [](size_t mapping_index, Path& p) {
        string json2 = pb2json(p);
#pragma omp critical(cout)
        cout<<json2<<endl;
    };

    function<void(size_t, Path&)>* callback = &paths_to_seqs;
    if (!as_seqs) {
        callback = &paths_to_json;
    }

    auto noop = [](NodeTraversal) { }; // don't handle the failed regions of the graph yet

    if (node_id) {

        graph->for_each_kpath_of_node(graph->get_node(node_id),
                path_only,
                max_length,
                edge_max,
                noop, noop,
                *callback);
    } else {
        graph->for_each_kpath_parallel(max_length,
                path_only,
                edge_max,
                noop, noop,
                *callback);
    }

    delete graph;

    return 0;

}

// Register subcommand
static Subcommand vg_paths("paths", "traverse paths in the graph", main_paths);

