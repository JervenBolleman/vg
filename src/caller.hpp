#ifndef VG_CALLER_HPP_INCLUDED
#define VG_CALLER_HPP_INCLUDED

#include <iostream>
#include <algorithm>
#include <functional>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <tuple>
#include "vg.pb.h"
#include "vg.hpp"
#include "hash_map.hpp"
#include "utility.hpp"
#include "pileup.hpp"
#include "path_index.hpp"
#include "genotypekit.hpp"
#include "option.hpp"

namespace vg {

using namespace std;

// container for storing pairs of support for calls (value for each strand)
struct StrandSupport {
    int fs; // forward support
    int rs; // reverse support
    double qual; // phred score (derived from sum log p-err over all observations)
    StrandSupport(int f = 0, int r = 0, double q = 0) :
        fs(f), rs(r), qual(q) {}
    bool operator<(const StrandSupport& other) const {
        if ((fs + rs) == (other.fs + other.rs)) {
            // more strand bias taken as less support
            return abs(fs - rs) > abs(other.fs - rs);
        } 
        return fs + rs < other.fs + other.rs;
    }
    bool operator>=(const StrandSupport& other) const {
        return !(*this < other);
    }
    bool operator==(const StrandSupport& other) const {
        return fs == other.fs && rs == other.rs && qual == other.qual;
    }
    // min out at 0
    StrandSupport operator-(const StrandSupport& other) const {
        return StrandSupport(max(0, fs - other.fs), max(0, rs - other.rs),
                             max(0., qual - other.qual));
    }
    StrandSupport& operator+=(const StrandSupport& other) {
        fs += other.fs;
        rs += other.rs;
        qual += other.qual;
        return *this;
    }
    int total() { return fs + rs; }
};

inline StrandSupport minSup(vector<StrandSupport>& s) {
    if (s.empty()) {
        return StrandSupport();
    }
    return *min_element(s.begin(), s.end());
}
inline StrandSupport maxSup(vector<StrandSupport>& s) {
    if (s.empty()) {
        return StrandSupport();
    }
    return *max_element(s.begin(), s.end());
}
inline StrandSupport avgSup(vector<StrandSupport>& s) {
    StrandSupport ret;
    if (!s.empty()) {
        for (auto sup : s) {
            ret.fs += sup.fs;
            ret.rs += sup.rs;
            ret.qual += sup.qual;
        }
        ret.fs /= s.size();
        ret.rs /= s.size();
        ret.qual /= s.size();
    }
    return ret;
}
inline StrandSupport totalSup(vector<StrandSupport>& s) {
    StrandSupport ret;
    if (!s.empty()) {
        for (auto sup : s) {
            ret.fs += sup.fs;
            ret.rs += sup.rs;
            ret.qual += sup.qual;
        }
    }
    return ret;
}

inline ostream& operator<<(ostream& os, const StrandSupport& sup) {
    return os << sup.fs << ", " << sup.rs << ", " << sup.qual;
}

// We need to break apart nodes but remember where they came from to update edges.
// Wrap all this up in this class.  For a position in the input graph, we can have
// up to three nodes in the augmented graph (Ref, Alt1, Alt2), so we map to node
// triplets (Entry struct below).  Note we never *call* all three nodes due to
// diploid assumption, but the augmented graph stores everything. 
struct NodeDivider {
    // up to three fragments per position in augmented graph (basically a Node 3-tuple,
    // avoiding aweful C++ tuple syntax)
    enum EntryCat {Ref = 0, Alt1, Alt2, Last};
    struct Entry { Entry(Node* r = 0, vector<StrandSupport> sup_r = vector<StrandSupport>(),
                         Node* a1 = 0, vector<StrandSupport> sup_a1 = vector<StrandSupport>(),
                         Node* a2 = 0, vector<StrandSupport> sup_a2 = vector<StrandSupport>()) : ref(r), alt1(a1), alt2(a2),
                                                               sup_ref(sup_r), sup_alt1(sup_a1), sup_alt2(sup_a2){}
        Node* ref; Node* alt1; Node* alt2;
        vector<StrandSupport> sup_ref;
        vector<StrandSupport> sup_alt1;
        vector<StrandSupport> sup_alt2;
        Node*& operator[](int i) {
            assert(i >= 0 && i <= 2);
            return i == EntryCat::Ref ? ref : (i == EntryCat::Alt1 ? alt1 : alt2);
        }
        vector<StrandSupport>& sup(int i) {
            assert(i >= 0 && i <= 2);
            return i == EntryCat::Ref ? sup_ref : (i == EntryCat::Alt1 ? sup_alt1 : sup_alt2);
        }
    };
    // offset in original graph node -> up to 3 nodes in call graph
    typedef map<int, Entry> NodeMap;
    // Node id in original graph to map above
    typedef hash_map<int64_t, NodeMap> NodeHash;
    NodeHash index;
    int64_t* _max_id;
    // map given node to offset i of node with id in original graph
    // this function can never handle overlaps (and should only be called before break_end)
    void add_fragment(const Node* orig_node, int offset, Node* subnode, EntryCat cat, vector<StrandSupport> sup);
    // break node if necessary so that we can attach edge at specified side
    // this function wil return NULL if there's no node covering the given location
    Entry break_end(const Node* orig_node, VG* graph, int offset, bool left_side);
    // assuming input node is fully covered, list of nodes that correspond to it in call graph
    // if node not in structure at all, just return input (assumption uncalled nodes kept as is)
    list<Mapping> map_node(int64_t node_id, int64_t start_offset, int64_t length, bool reverse);
    // erase everything (but don't free any Node pointers, they belong to the graph)
    void clear();
};
ostream& operator<<(ostream& os, const NodeDivider::NodeMap& nm);
ostream& operator<<(ostream& os, NodeDivider::Entry entry);

/**
 * Super simple graph augmentor/caller.
 * Idea: Idependently process Pileup records, using simple model to make calls that
 *       take into account read errors with diploid assumption.  Edges and node positions
 *       are called independently for now.  
 * Outputs either a sample graph (only called nodes and edges) or augmented graph
 * (include uncalled nodes and edges too).
 */
class Caller {
public:

    // log of zero
    static const double Log_zero;
    // use this score when pileup is missing quality
    static const char Default_default_quality;
    // don't augment graph without minimum support
    static const int Default_min_aug_support;
    
    Caller(VG* graph,
           int default_quality = Default_default_quality,
           int min_aug_support = Default_min_aug_support);

   ~Caller();
    void clear();

    // input graph
    VG* _graph;
    // Output augmented graph with annotations
    AugmentedGraph _augmented_graph;

    // buffer for base calls for each position in the node
    // . = reference
    // - = missing
    typedef pair<string, string> Genotype;
    vector<Genotype> _node_calls;
    vector<pair<StrandSupport, StrandSupport> > _node_supports;
    // separate structure for isnertion calls since they
    // don't really have reference coordinates (instead happen just to
    // right of offset).  
    vector<Genotype> _insert_calls;
    vector<pair<StrandSupport, StrandSupport> > _insert_supports;
    // buffer for current node;
    const Node* _node;
    // max id in call_graph
    int64_t _max_id;
    // link called nodes back to the original graph. needed
    // to figure out where edges go
    NodeDivider _node_divider;
    unordered_set<int64_t> _visited_nodes;
    unordered_map<pair<NodeSide, NodeSide>, StrandSupport> _called_edges; // map to support
    // deletes can don't necessarily need to be in incident to node ends
    // so we throw in an offset into the mix. 
    typedef pair<NodeSide, int> NodeOffSide;
    // map a call category to an edge
    typedef unordered_map<pair<NodeOffSide, NodeOffSide>, char> EdgeHash;
    EdgeHash _augmented_edges;
    // keep track of inserted nodes for tsv output
    struct InsertionRecord {
        Node* node;
        StrandSupport sup;
        int64_t orig_id;
        int orig_offset;
    };
    typedef unordered_map<int64_t, InsertionRecord> InsertionHash;
    InsertionHash _inserted_nodes;
    // hack for better estimating support for edges that go around
    // insertions (between the adjacent ref nodes)
    typedef unordered_map<pair<NodeOffSide, NodeOffSide>, StrandSupport> EdgeSupHash;
    EdgeSupHash _insertion_supports;

    // need to keep track of support for augmented deletions
    // todo: generalize augmented edge support
    EdgeSupHash _deletion_supports;

    // maximum number of nodes to call before writing out output stream
    int _buffer_size;
    // if we don't have a mapping quality for a read position, use this
    char _default_quality;
    // minimum support to augment graph
    int _min_aug_support;

    // write the augmented graph
    void write_augmented_graph(ostream& out, bool json);

    // call every position in the node pileup
    void call_node_pileup(const NodePileup& pileup);

    // call an edge.  remembering it in a table for the whole graph
    void call_edge_pileup(const EdgePileup& pileup);

    // fill in edges in the augmented graph (those that are incident to 2 call
    // nodes) and add uncalled nodes (optionally)
    void update_augmented_graph();

    // map paths from input graph into called graph
    void map_paths();
    // make sure mapped paths generate same strings as input paths
    void verify_path(const Path& in_path, const list<Mapping>& call_path);
    
    // call position at given base
    // if insertion flag set to true, call insertion between base and next base
    void call_base_pileup(const NodePileup& np, int64_t offset, bool insertions);
    
    // Find the top-two bases in a pileup, along with their counts
    // Last param toggles whether we consider only inserts or everything else
    // (do not compare all at once since inserts do not have reference coordinates)
    void compute_top_frequencies(const BasePileup& bp,
                                 const vector<pair<int64_t, int64_t> >& base_offsets,
                                 string& top_base, int& top_count, int& top_rev_count,
                                 string& second_base, int& second_count, int& second_rev_count,
                                 int& total_count, bool inserts);

    // Sum up the qualities of a given symbol in a pileup
    double total_base_quality(const BasePileup& pb,
                              const vector<pair<int64_t, int64_t> >& base_offsets,
                              const string& val);

    // write graph structure corresponding to all the calls for the current
    // node.  
    void create_node_calls(const NodePileup& np);

    void create_augmented_edge(Node* node1, int from_offset, bool left_side1, bool aug1,
                               Node* node2, int to_offset, bool left_side2, bool aug2, char cat,
                               StrandSupport support);

    // Annotate nodes and edges in the augmented graph with call info.
    void annotate_augmented_node(Node* node, char call, StrandSupport support, int64_t orig_id, int orig_offset);
    void annotate_augmented_edge(Edge* edge, char call, StrandSupport support);
    void annotate_augmented_nd();

    // log function that tries to avoid 0s
    static double safe_log(double v) {
        return v == 0. ? Log_zero : ::log10(v);
    }

    // call missing
    static bool missing_call(const Genotype& g) {
        return g.first == "-" &&  g.second == "-";
    }

    // call is reference
    static bool ref_call(const Genotype& g) {
        return g.first == "." && (g.second == "." || g.second == "-");
    }

    // classify call as 0: missing 1: reference 2: snp
    // (holdover from before indels)
    static int call_cat(const Genotype&g) {
        if (missing_call(g)) {
            return 0;
        } else if (ref_call(g)) {
            return 1;
        }
        return 2;
    }
};

ostream& operator<<(ostream& os, const Caller::NodeOffSide& no);

/**
 * Call2Vcf: take an augmented graph from a Caller and produce actual calls in a
 * VCF.
 */
class Call2Vcf : public Configurable {

public:

    /**
     * Set up to call with default parameters.
     */
    Call2Vcf() = default;
    
    /**
     * We use this to represent a contig in the primary path, with its index and coverage info.
     */
    class PrimaryPath {
    public:
        /**
         * Index the given path in the given augmented graph, and compute all
         * the coverage bin information with the given bin size.
         */
        PrimaryPath(AugmentedGraph& augmented, const string& ref_path_name, size_t ref_bin_size); 
    
        /**
         * Get the support at the bin appropriate for the given primary path
         * offset.
         */
        const Support& get_support_at(size_t primary_path_offset) const;
        
        /**
         * Get the index of the bin that the given path position falls in.
         */
        size_t get_bin_index(size_t primary_path_offset) const;
    
        /**
         * Get the bin with minimal coverage.
         */
        size_t get_min_bin() const;
    
        /**
         * Get the bin with maximal coverage.
         */
        size_t get_max_bin() const;
    
        /**
         * Get the support in the given bin.
         */
        const Support& get_bin(size_t bin) const;
        
        /**
         * Get the total number of bins that the path is divided into.
         */
        size_t get_total_bins() const;
        
        /**
         * Get the average support over the path.
         */
        Support get_average_support() const;
        
        /**
         * Get the average support over a collection of paths.
         */
        static Support get_average_support(const map<string, PrimaryPath>& paths);
        
        /**
         * Get the total support for the path.
         */
        Support get_total_support() const;
    
        /**
         * Get the PathIndex for this primary path.
         */
        PathIndex& get_index();
        
        /**
         * Get the PathIndex for this primary path.
         */
        const PathIndex& get_index() const;
        
        /**
         * Gets the path name we are representing.
         */
        const string& get_name() const;
        
    protected:
        /// How wide is each coverage bin along the path?
        size_t ref_bin_size;
        
        /// This holds the index for this path
        PathIndex index;
        
        /// This holds the name of the path
        string name;
        
        /// What's the expected in each bin along the path? Coverage gets split
        /// evenly over both strands.
        vector<Support> binned_support;
        
        /// Which bin has min support?
        size_t min_bin;
        /// Which bin has max support?
        size_t max_bin;
        
        /// What's the total Support over every bin?
        Support total_support;
    };
    
    
    /**
     * Produce calls for the given annotated augmented graph. If a
     * pileup_filename is provided, the pileup is loaded again and used to add
     * comments describing variants
     */
    void call(AugmentedGraph& augmented, string pileup_filename = "");

    /** 
     * Get the support and size for each traversal in a list. Discount support
     * of minus_traversal if it's specified.  Use average_support_switch_threshold and
     * use_average_support to decide whether to return min or avg supports
     */
    tuple<vector<Support>, vector<size_t> > get_traversal_supports_and_sizes(
        AugmentedGraph& augmented, SnarlManager& snarl_manager, const Snarl& site,
        const vector<SnarlTraversal>& traversals,
        const SnarlTraversal* minus_traversal = NULL);


    /**
     * For the given snarl, find the reference traversal, the best traversal,
     * and the second-best traversal, recursively, if any exist. These
     * traversals will be fully filled in with nodes.
     *
     * Only snarls which are ultrabubbles can be called.
     *
     * Expects the given baseline support for a diploid call.
     *
     * Will not return more than 1 + copy_budget SnarlTraversals, and will
     * return less if some copies are called as having the same traversal.
     *
     * Does not deduplicate agains the ref traversal; it may be the same as the
     * best or second-best.
     *
     * Uses the given copy number allowance, and emits a Locus for this Snarl
     * and any child Snarls.
     *
     * If no path through the Snarl can be found, emits no Locus and returns no
     * SnarlTraversals.
     */
    vector<SnarlTraversal> find_best_traversals(AugmentedGraph& augmented,
        SnarlManager& snarl_manager, TraversalFinder* finder, const Snarl& site,
        const Support& baseline_support, size_t copy_budget,
        function<void(const Locus&, const Snarl*)> emit_locus);
    
    /**
     * Decide if the given SnarlTraversal is included in the original base graph
     * (true), or if it represents a novel variant (false).
     *
     * Looks at the nodes in the traversal, and sees if their calls are
     * CALL_REFERENCE or not.
     *
     * Handles single-edge traversals.
     *
     */
    bool is_reference(const SnarlTraversal& trav, AugmentedGraph& augmented);
    
    /**
     * Decide if the given Path is included in the original base graph (true) or
     * if it represents a novel variant (false).
     *
     * Looks at the nodes, and sees if their calls are CALL_REFERENCE or not.
     *
     * The path can't be empty; it has to be anchored to something (probably the
     * start and end of the snarl it came from).
     */
    bool is_reference(const Path& path, AugmentedGraph& augmented);
    
    /**
     * Find the primary path, if any, that the given site is threaded onto.
     *
     * TODO: can only work by brute-force search.
     */
    map<string, PrimaryPath>::iterator find_path(const Snarl& site, map<string, PrimaryPath>& primary_paths);

    /** 
     * Get the amount of support.  Can use this function to toggle between unweighted (total from genotypekit)
     * and quality-weighted (support_quality below) in one place.
     */
    function<double(const Support&)> support_val;

    static double support_quality(const Support& support) {
        return support.quality();
    }
    
    // Option variables
    
    /// Should we output in VCF (true) or Protobuf Locus (false) format?
    Option<bool> convert_to_vcf{this, "no-vcf", "V", true,
        "output variants in binary Loci format instead of text VCF format"};
    /// How big should our output buffer be?
    size_t locus_buffer_size = 1000;
    
    /// What are the names of the reference paths, if any, in the graph?
    Option<vector<string>> ref_path_names{this, "ref", "r", {},
        "use the path with the given name as a reference path (can repeat)"};
    /// What name should we give each contig in the VCF file? Autodetected from
    /// path names if empty or too short.
    Option<vector<string>> contig_name_overrides{this, "contig", "c", {},
        "use the given name as the VCF name for the corresponding reference path (can repeat)"};
    /// What should the total sequence length reported in the VCF header be for
    /// each contig? Autodetected from path lengths if empty or too short.
    Option<vector<size_t>> length_overrides{this, "length", "l", {},
        "override total sequence length in VCF for the corresponding reference path (can repeat)"};
    /// What name should we use for the sample in the VCF file?
    Option<string> sample_name{this, "sample", "S", "SAMPLE",
        "name the sample in the VCF with the given name"};
    /// How far should we offset positions of variants?
    Option<int64_t> variant_offset{this, "offset", "o", 0,
        "offset variant positions by this amount in VCF"};
    /// How many nodes should we be willing to look at on our path back to the
    /// primary path? Keep in mind we need to look at all valid paths (and all
    /// combinations thereof) until we find a valid pair.
    Option<int64_t> max_search_depth{this, "max-search-depth", "D", 1000,
        "maximum depth for path search"};
    /// How many search states should we allow on the DFS stack when searching
    /// for traversals?
    Option<int64_t> max_search_width{this, "max-search-width", "wWmMsS", 1000,
        "maximum width for path search"};
    
    
    /// What fraction of average coverage should be the minimum to call a
    /// variant (or a single copy)? Default to 0 because vg call is still
    /// applying depth thresholding
    Option<double> min_fraction_for_call{this, "min-cov-frac", "F", 0,
        "min fraction of average coverage at which to call"};
    /// What fraction of the reads supporting an alt are we willing to discount?
    /// At 2, if twice the reads support one allele as the other, we'll call
    /// homozygous instead of heterozygous. At infinity, every call will be
    /// heterozygous if even one read supports each allele.
    Option<double> max_het_bias{this, "max-het-bias", "H", 10,
        "max imbalance factor to call heterozygous, alt major on SNPs"};
    /// Like above, but applied to ref / alt ratio (instead of alt / ref)
    Option<double> max_ref_het_bias{this, "max-ref-bias", "R", 4.5,
        "max imbalance factor to call heterozygous, ref major"};
    /// Like the max het bias, but applies to novel indels.
    Option<double> max_indel_het_bias{this, "max-indel-het-bias", "I", 3,
        "max imbalance factor to call heterozygous, alt major on indels"};
    /// Like the max het bias, but applies to multiallelic indels.
    Option<double> max_indel_ma_bias{this, "max-indel-ma-bias", "G", 6,
        "max imbalance factor between ref and alt2 to call 1/2 double alt on indels"};
    /// What's the minimum integer number of reads that must support a call? We
    /// don't necessarily want to call a SNP as het because we have a single
    // supporting read, even if there are only 10 reads on the site.
    Option<size_t> min_total_support_for_call{this, "min-count", "n", 1, 
        "min total supporting read count to call a variant"};
    /// Bin size used for counting coverage along the reference path.  The
    /// bin coverage is used for computing the probability of an allele
    /// of a certain depth
    Option<size_t> ref_bin_size{this, "bin-size", "B", 250,
        "bin size used for counting coverage"};
    /// On some graphs, we can't get the coverage because it's split over
    /// parallel paths.  Allow overriding here
    Option<double> expected_coverage{this, "avg-coverage", "C", 0.0,
        "specify expected coverage (instead of computing on reference)"};
    /// Should we use average support instead of minimum support for our
    /// calculations?
    Option<bool> use_average_support{this, "use-avg-support", "u", false,
        "use average instead of minimum support"};
    /// Max traversal length threshold at which we switch from minimum support
    /// to average support (so we don't use average support on pairs of adjacent
    /// errors and miscall them, but we do use it on long runs of reference
    /// inside a deletion where the min support might not be representative.
    Option<size_t> average_support_switch_threshold{this, "use-avg-support-above", "uUaAtT", 100,
        "use average instead of minimum support for sites this long or longer"};
    
    /// What's the maximum number of bubble path combinations we can explore
    /// while finding one with maximum support?
    size_t max_bubble_paths = 100;
    /// what's the minimum ref or alt allele depth to give a PASS in the filter
    /// column? Also used as a min actual support for a second-best allele call
    Option<size_t> min_mad_for_filter{this, "min-mad", "E", 5,
        "min. ref/alt allele depth to PASS filter or be a second-best allele"};
    /// what's the maximum total depth to give a PASS in the filter column
    Option<size_t> max_dp_for_filter{this, "max-dp", "MmDdAaXxPp", 0,
        "max depth to PASS filter (0 for unlimited)"};
    /// what's the maximum total depth to give a PASS in the filter column, as a
    /// multiple of the global baseline coverage?
    Option<double> max_dp_multiple_for_filter{this, "max-dp-multiple", "MmDdAaXxPp", 0,
        "max portion of global expected depth to PASS filter (0 for unlimited)"};
    /// what's the maximum total depth to give a PASS in the filter column, as a
    /// multiple of the local baseline coverage?
    Option<double> max_local_dp_multiple_for_filter{this, "max-local-dp-multiple", "MmLlOoDdAaXxPp", 0,
        "max portion of local expected depth to PASS filter (0 for unlimited)"};
    /// what's the min log likelihood for allele depth assignments to PASS?
    Option<double> min_ad_log_likelihood_for_filter{this, "min-ad-log-likelihood", "MmAaDdLliI", -9.0,
        "min log likelihood for AD assignments to PASS filter (0 for unlimited)"};
        
    Option<bool> write_trivial_calls{this, "trival", "ivtTIRV", false,
        "write trivial vcf calls (ex 0/0 genotypes)"};
        
    /// Should we call on nodes/edges outside of snarls by coverage (true), or
    /// just assert that primary path things exist and off-path things don't
    /// (false)?
    Option<bool> call_other_by_coverage{this, "call-nodes-by-coverage", "cCoObB", false,
        "make calls on nodes/edges outside snarls by coverage"};

    /// Use total support count (true) instead of total support quality (false) when choosing
    /// top alleles and deciding gentypes based on the biases.  
    Option<bool> use_support_count{this, "use-support-count", "T", false,
        "use total support count instead of total support quality for selecting top alleles"};
    
    /// print warnings etc. to stderr
    bool verbose = false;
    
};

}

#endif
