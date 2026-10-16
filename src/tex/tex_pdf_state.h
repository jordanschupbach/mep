#ifndef MEP_TEX_PDF_STATE_H
#define MEP_TEX_PDF_STATE_H

// What mep-tex keeps about the PDF it is making, beside TeX's own memory:
// the objects pdfTeX's primitives create (\pdfobj, \pdfxform, \pdfximage,
// annotations, links, destinations, outlines), the data of the whatsit
// nodes that refer to them, and the document-level strings (\pdfinfo,
// \pdfcatalog ...). Object numbers are the PDF's own and are given out
// when an object is created, because documents use them at once
// (`\the\pdflastobj\space 0 R` in a literal).

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace meptex {

struct PdfContent;  // a content stream being made (tex_pdf_out.cpp)

struct PdfObject {
    enum Kind { Raw, Stream, Xform, Ximage, Annot, Link, Dest, Thread, Outline, Page, Font, Other };
    Kind kind = Raw;
    int32_t num = 0;          // the PDF object number
    bool reserved = false;    // \pdfobj reserveobjnum: number given, no content yet
    bool written = false;     // already in the output file
    bool referenced = false;  // \pdfrefobj'd (a non-immediate object is written only then)
    bool immediate = false;
    std::string content;      // the object's text, or a stream's data
    std::string attr;         // a stream's or form's extra dictionary entries
    std::string resources;    // a form's \pdfxform resources
    bool from_file = false;   // \pdfobj file: content names a file to read
    // forms and images
    int32_t box = 0;          // \pdfxform: the box (in TeX's memory) holding the form
    int32_t width = 0, height = 0, depth = 0;  // scaled points
    std::string file;         // \pdfximage: the file
    int32_t page = 1;         // \pdfximage page n
    int32_t pages = 1;        // pages in that file
    int32_t colordepth = 0;
    bool image_ok = false;    // the image file was read
    int32_t bbox[4] = {0, 0, 0, 0};
};

// One pdfTeX whatsit's data (the node holds its index, mem[p+4]).
struct PdfNodeData {
    bool used = false;
    int32_t mode = 0;          // literal: 0 origin, 1 page, 2 direct, 3 raw; colorstack: command
    bool shipout = false;      // \pdfliteral shipout: expanded when shipped
    int32_t obj = 0;           // the object this node refers to or makes
    int32_t stack = 0;         // \pdfcolorstack's stack
    std::string text;          // an action spec, a destination name ...
    int32_t num = 0;           // a destination number, a thread id ...
    bool named = false;        // destination/thread given by name
    int32_t dest_type = 0;     // xyz, fit, fith, fitv, fitb, fitbh, fitbv, fitr
    int32_t zoom = 0;          // xyz zoom (0 = none)
};

struct PdfColorStack {
    bool page_start = false;   // `page`: its current value is set again at the start of each page
    bool direct = false;
    std::vector<std::string> stack;
    std::string initial;
};

struct PdfOutline {
    std::string title;
    std::string attr;
    std::string action;
    int32_t count = 0;
    int32_t obj = 0;
};

// A font as the PDF has it: one per TFM name (every size of cmr10 shares
// one), with the characters used anywhere in the document.
struct PdfFont {
    std::string tfm;           // the TFM name: the map file's key
    int32_t obj = 0;           // the /Font dictionary
    int32_t res = 0;           // its resource name is /F<res>
    int32_t tex_font = 0;      // a TeX font with this TFM (for widths and parameters)
    bool used[256] = {};
};

// One line of a map file (pdftex.map): which Type 1 file draws a TFM.
struct PdfMapEntry {
    std::string tfm, ps_name, font_file, enc_file;
    double slant = 0, extend = 1;
    bool embed = true;
};

struct PdfState {
    PdfState() {
        PdfColorStack c;  // stack 0 is pdfTeX's own: \pdfcolorstack 0 is always there
        c.page_start = true;
        c.direct = true;
        c.initial = "0 g 0 G";
        colorstacks.push_back(c);
    }
    int32_t next_obj = 1;  // the next PDF object number to give out
    std::vector<PdfObject> objects;          // by object number (index 0 unused)
    std::vector<PdfNodeData> nodes;          // whatsit data
    std::vector<int32_t> free_nodes;
    int32_t last_obj = 0, last_xform = 0, last_ximage = 0, last_ximage_pages = 0, last_annot = 0, last_link = 0;
    int32_t last_ximage_colordepth = 0, retval = 0;
    int32_t last_x_pos = 0, last_y_pos = 0;
    std::string info, catalog, names, trailer, trailer_id, catalog_openaction;
    std::vector<PdfOutline> outlines;
    std::vector<std::string> map_lines;     // \pdfmapline / \pdfmapfile, in order
    std::vector<PdfColorStack> colorstacks;
    std::unordered_map<std::string, std::string> glyph_to_unicode;
    // \pdfmatch
    std::vector<std::pair<int32_t, std::string>> matches;
    // timer and random numbers
    double timer_start = 0;
    uint64_t random_seed = 0;
    uint64_t random_state = 0;
    bool random_init = false;
    // font character codes (\lpcode ...), by (code, font, char)
    std::unordered_map<int64_t, int32_t> font_char_codes;
    std::unordered_map<int32_t, std::string> font_attrs;
    // page objects, by page number, made on demand by \pdfpageref
    std::map<int32_t, int32_t> page_objs;

    // The file being written (tex_pdf_out.cpp): all of it is kept in
    // memory and written out when the job ends.
    bool started = false;               // the header is out: this job makes a PDF
    std::string out;
    std::vector<int64_t> offsets;       // by object number; -1 = not written
    int32_t total_pages = 0;
    int32_t pages_obj = 0;              // the page tree's root
    std::vector<int32_t> page_list;     // page objects, in order
    std::map<std::string, int32_t> font_by_tfm;  // TFM name -> index in fonts
    std::vector<PdfFont> fonts;
    bool map_loaded = false;
    std::unordered_map<std::string, PdfMapEntry> map;
    // destinations: by name and by number (\pdfdest num n)
    std::map<std::string, int32_t> named_dests;
    std::map<int32_t, int32_t> num_dests;
    // dvipdfmx's pdf: specials (\special{pdf:obj @name ...}): named objects,
    // their bodies (written at the end, when every @name has a number),
    // and what `put` adds to the catalog and the document information
    std::map<std::string, int32_t> special_names;
    std::map<int32_t, std::string> special_objs;
    std::map<int32_t, std::pair<std::string, std::string>> special_streams;  // obj -> (dictionary entries, data)
    int32_t catalog_obj = 0;
    std::string catalog_extra, info_extra;
    bool warned_special = false;
};

}  // namespace meptex

#endif
