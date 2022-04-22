#include "Rcpp.h"

#include "kaori/handlers/CombinatorialBarcodesSingleEnd.hpp"
#include "kaori/process_data.hpp"
#include "byteme/SomeFileReader.hpp"

#include <algorithm>
#include <vector>

#include "utils.h"

template<size_t N, size_t V, class Reader>
Rcpp::List count_combo_barcodes_single_(
    Reader& reader, 
    std::string constant, 
    int strand, 
    const std::vector<std::vector<const char*> >& options,
    int mismatches, 
    bool use_first, 
    int nthreads) 
{
    kaori::CombinatorialBarcodesSingleEnd<N, V> handler(constant.c_str(), constant.size(), strand, options, mismatches);
    handler.set_first(use_first);
    kaori::process_single_end_data(&reader, handler, nthreads);
    handler.sort();

    auto output = count_combinations(handler.get_combinations(), 1);
    output[2] = Rcpp::IntegerVector::create(handler.get_total());
    return output;
}

//[[Rcpp::export(rng=false)]]
Rcpp::List count_combo_barcodes_single(std::string path, std::string constant, int strand, Rcpp::List options, int mismatches, bool use_first, int nthreads) {
    byteme::SomeFileReader reader(path.c_str());

    std::vector<std::vector<const char*> > opts;
    std::vector<Rcpp::CharacterVector> converted;
    for (size_t o = 0; o < options.size(); ++o) {
        converted.push_back(Rcpp::CharacterVector(options[o]));
        opts.push_back(format_pointers(converted.back()));
    }

    // Support up to 256 bp constant regions.
    Rcpp::List output;
    if (constant.size() <= 32) {
        output = count_combo_barcodes_single_<128, 2>(reader, constant, strand, opts, mismatches, use_first, nthreads);
    } else if (constant.size() <= 64) {
        output = count_combo_barcodes_single_<256, 2>(reader, constant, strand, opts, mismatches, use_first, nthreads);
    } else if (constant.size() <= 128) {
        output = count_combo_barcodes_single_<512, 2>(reader, constant, strand, opts, mismatches, use_first, nthreads);
    } else if (constant.size() <= 256) {
        output = count_combo_barcodes_single_<1024, 2>(reader, constant, strand, opts, mismatches, use_first, nthreads);
    } else {
        throw std::runtime_error("lacking compile-time support for constant regions longer than 256 bp");
    }

    return output;
}