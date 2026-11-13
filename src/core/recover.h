// Damaged files: what qpdf's own reconstruction loses and a careful reader still finds.
#pragma once

#include <qpdf/QPDF.hh>

#include <map>
#include <string>

namespace eqx {

// Scans the file bytes for every top-level object and every object inside an object stream and appends a new
// cross-reference stream, so that a fresh qpdf reads the file with all of them. The last definition in the file wins.
// `trailer` is the trailer qpdf found; a null handle when it found none, then the last one in the bytes counts. Empty when
// nothing usable came out. ISO 32000-1 7.5.4, 7.5.7, 7.5.8.
std::string recovered_image(const std::string& bytes, QPDFObjectHandle trailer);

// A valid hybrid-reference file can hide objects: free in the table, listed in the /XRefStm of the same section (ISO 32000-1
// 7.5.8.4). qpdf takes the free entry and never finds them. Returns the file with one more cross-reference stream on top
// that lists exactly those objects, empty when the file has none. `known`: object numbers qpdf found.
std::string hidden_objects_image(const std::string& bytes, QPDFObjectHandle trailer, const std::map<QPDFObjGen, QPDFXRefEntry>& known);

}  // namespace eqx
