#include "Rcpp.h"

#include "hash_sequence.h"
#include "build_hash.h"
#include <stdexcept>
#include <vector>
#include <sstream>
#include <map>

/* Combination guide parser. */

template<size_t nvariable>
struct combination {
    int indices[nvariable];
    bool operator<(const combination<nvariable>& other) const {
        for (size_t i=0; i<nvariable; ++i) {
            if (indices[i] < other.indices[i]) { return true; }
            if (indices[i] > other.indices[i]) { return false; }
        }
        return false;
    }
};

template<size_t nvariable>
struct se_combo_info {
    se_combo_info(Rcpp::List, Rcpp::StringVector);
    std::vector<seqhash> variable_hashes;
    std::vector<std::u32string> constant_hash;
    std::vector<int> constant_lengths, variable_lengths;
    std::vector<size_t> constant_starts, variable_starts;
    size_t total_len;

    std::map<combination<nvariable>, int> out_store;
};

template<size_t nvariable>
se_combo_info<nvariable>::se_combo_info(Rcpp::List Guides, Rcpp::StringVector Constants) {
    // Setting up the guides.
    if (nvariable!=Guides.size()) {
        std::stringstream err;
        err << "expecting " << nvariable << " variable regions";
        throw std::runtime_error(err.str());
    }
    variable_hashes.reserve(nvariable);
    variable_lengths.reserve(nvariable);

    for (size_t g=0; g<nvariable; ++g) {
        Rcpp::StringVector guides(Guides[g]);
        auto hash=build_hash(guides);
        variable_hashes.push_back(std::move(hash.first));
        
        if (hash.second.size()!=1) {
            throw std::runtime_error("all sequences should be of the same length");
        }
        variable_lengths.push_back(hash.second[0]);
    }

    // Setting up the constnt regions.
    const size_t nconstant=nvariable+1;
    if (Constants.size()!=nvariable+1) {
        throw std::runtime_error("number of constant regions should be 1 more than variable regions");
    }

    constant_hash.reserve(nconstant);
    constant_lengths.reserve(nconstant);

    for (size_t s=0; s<nconstant; ++s) {
        Rcpp::String con(Constants[s]);
        const size_t len=Rf_length(con.get_sexp());
        const char* ptr=con.get_cstring();
        constant_hash.push_back(hash_sequence(ptr, len)); 
        constant_lengths.push_back(len);
    }

    // Setting up search parameters.
    constant_starts.resize(nconstant);
    variable_starts.resize(nvariable);
    total_len=constant_lengths[0];

    for (size_t i=0; i<nvariable; ++i) {
        variable_starts[i]=total_len;
        total_len+=variable_lengths[i];
        constant_starts[i+1]=total_len;
        total_len+=constant_lengths[i+1];
    }

    return;
}

template<size_t nvariable>
SEXP setup_barcodes_combo(SEXP constants, SEXP guide_list) {
    se_combo_info<nvariable> * ptr = new se_combo_info<nvariable>(guide_list, constants);
    return Rcpp::XPtr<se_combo_info<nvariable> >(ptr, true);
}

template<size_t nvariable>
SEXP count_barcodes_combo(SEXP seqs, SEXP xptr) {
    Rcpp::XPtr<se_combo_info<nvariable> > ptr(xptr);
    const auto& variable_hashes=ptr->variable_hashes;
    const auto& constant_hash=ptr->constant_hash;
    const auto& variable_lengths=ptr->variable_lengths;
    const auto& constant_lengths=ptr->constant_lengths;
    const auto& variable_starts=ptr->variable_starts;
    const auto& constant_starts=ptr->constant_starts;

    const size_t total_len=ptr->total_len;
    const size_t nconstant=nvariable+1; // see above.
    auto& out_store=ptr->out_store;

    // Running through the sequences and matching it to the guides.
    Rcpp::StringVector Seqs(seqs);

    for (size_t i=0; i<Seqs.size(); ++i) {
        Rcpp::String s=Seqs[i];
        const char* sptr=s.get_cstring();
        const size_t len=Rf_length(s.get_sexp());
        if (len < total_len) { break; }

        // Setting up the scanners.
        std::vector<hash_scanner> constant_scan, variable_scan;
        constant_scan.reserve(nconstant);
        for (size_t j=0; j<nconstant; ++j) {
            constant_scan.push_back(hash_scanner(sptr+constant_starts[j], constant_lengths[j]));
        }
        variable_scan.reserve(nvariable);
        for (size_t j=0; j<nvariable; ++j) {
            variable_scan.push_back(hash_scanner(sptr+variable_starts[j], variable_lengths[j]));
        }

        // Traversing through the sequence.
        size_t end=total_len;
        do {
            bool is_valid=true;
            for (auto& x : constant_scan) { is_valid &= x.valid(); } 
            for (auto& x : variable_scan) { is_valid &= x.valid(); }

            if (is_valid) {
                bool is_equal=true;
                for (size_t j=0; j<nconstant; ++j) {
                    if (constant_hash[j]!=constant_scan[j].hash()) {
                        is_equal=false;
                        break;
                    }
                }

                if (is_equal) {
                    bool has_match=true;
                    combination<nvariable> tmp;

                    for (size_t j=0; j<nvariable; ++j) {
                        const auto& curhash=variable_hashes[j];
                        auto it=curhash.find(variable_scan[j].hash());
                        if (it!=curhash.end()) {
                            tmp.indices[j]=it->second;
                        } else {
                            has_match=false;
                            break;
                        }
                    }

                    if (has_match) { 
                        // Int values in map get value initialized to zero.
                        ++out_store[tmp];
                        break; 
                    }
                }
            }

            if (end >= len) {
                break;
            } 

            for (auto& x : constant_scan) { x.advance(); }
            for (auto& x : variable_scan) { x.advance(); }
            ++end;
        } while (1);
    }

    return R_NilValue;
}

template<size_t nvariable>
SEXP report_barcodes_combo(SEXP xptr) {
    Rcpp::XPtr<se_combo_info<nvariable> > ptr(xptr);
    const auto& out_store=ptr->out_store;

    std::vector<Rcpp::IntegerVector> keys(nvariable);
    for (auto& k : keys) { k = Rcpp::IntegerVector(out_store.size()); }
    Rcpp::IntegerVector counts(out_store.size());

    size_t i=0;
    for (const auto& pairing : out_store) {
        const auto& key=pairing.first;
        for (size_t j=0; j<nvariable; ++j) {
            keys[j][i]=key.indices[j]+1; // get back to 1-based indexing.
        }
        counts[i]=pairing.second; 
        ++i;
    }

    Rcpp::List output(2);
    output[0]=counts;
    output[1]=Rcpp::List(keys.begin(), keys.end());
    return output;
}

/****************************************************
 * Realizations of template functions for 2 guides. *
 ****************************************************/

// [[Rcpp::export(rng=false)]]
SEXP setup_barcodes_combo_dual(SEXP constants, SEXP guide_list) {
    return setup_barcodes_combo<2>(constants, guide_list);
}

// [[Rcpp::export(rng=false)]]
SEXP count_barcodes_combo_dual(SEXP seqs, SEXP xptr) {
    return count_barcodes_combo<2>(seqs, xptr);
}

// [[Rcpp::export(rng=false)]]
SEXP report_barcodes_combo_dual(SEXP xptr) {
    return report_barcodes_combo<2>(xptr);
}
