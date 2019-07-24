/*
 * fonts.cpp
 * Copyright (C) 2019 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "global.h"
#include <iostream>
#include <stack>

using namespace pdf;

static inline PyObject*
ref_as_tuple(const PdfReference &ref) {
    unsigned long num = ref.ObjectNumber(), generation = ref.GenerationNumber();
    return Py_BuildValue("kk", num, generation);
}

static inline PdfObject*
get_font_file(const PdfObject *descriptor) {
    PdfObject *ff = descriptor->GetIndirectKey("FontFile");
    if (!ff) ff = descriptor->GetIndirectKey("FontFile2");
    if (!ff) ff = descriptor->GetIndirectKey("FontFile3");
    return ff;
}

static inline void
remove_font(PdfVecObjects &objects, PdfObject *font) {
    PdfObject *descriptor = font->GetIndirectKey("FontDescriptor");
    if (descriptor) {
        const PdfObject *ff = get_font_file(descriptor);
        if (ff) delete objects.RemoveObject(ff->Reference());
        delete objects.RemoveObject(descriptor->Reference());
    }
    delete objects.RemoveObject(font->Reference());
}

static inline uint64_t
ref_as_integer(pdf_objnum num, pdf_gennum gen) {
    return static_cast<uint64_t>(num) | (static_cast<uint64_t>(gen) << 32);
}

static inline uint64_t
ref_as_integer(const PdfReference &ref) { return ref_as_integer(ref.ObjectNumber(), ref.GenerationNumber()); }


static inline void
replace_font_references(PDFDoc *self, std::unordered_map<uint64_t, uint64_t> &ref_map) {
    int num_pages = self->doc->GetPageCount();
    for (int i = 0; i < num_pages; i++) {
        PdfPage *page = self->doc->GetPage(i);
        PdfDictionary &resources = page->GetResources()->GetDictionary();
        PdfObject* f = resources.GetKey("Font");
        if (f && f->IsDictionary()) {
            const PdfDictionary &font = f->GetDictionary();
            PdfDictionary new_font = PdfDictionary(font);
            for (auto &k : font.GetKeys()) {
                if (k.second->IsReference()) {
                    uint64_t key = ref_as_integer(k.second->GetReference()), r;
                    try {
                        r = ref_map.at(key);
                    } catch (const std::out_of_range &err) { continue; }
                    PdfReference new_ref(static_cast<uint32_t>(r & 0xffffffff), r >> 32);
                    new_font.AddKey(k.first.GetName(), new_ref);
                }
            }
            resources.AddKey("Font", new_font);
        }
    }
}

static bool
used_fonts_in_page(PdfPage *page, unordered_reference_set &ans) {
    PdfContentsTokenizer tokenizer(page);
    bool in_text_block = false;
    const char* token = NULL;
    EPdfContentsType contents_type;
    PdfVariant var;
    std::stack<PdfVariant> stack;

    while (tokenizer.ReadNext(contents_type, token, var)) {
        if (contents_type == ePdfContentsType_Variant) stack.push(var);
        if (contents_type != ePdfContentsType_Keyword) continue;
        if (strcmp(token, "BT") == 0) {
            in_text_block = true;
            continue;
        } else if (strcmp(token, "ET") == 0) {
            in_text_block = false;
            continue;
        }
        if (!in_text_block) continue;
        if (strcmp(token, "Tf") == 0) {
            stack.pop();
            if (stack.size() > 0 && stack.top().IsName()) {
                const PdfName &reference_name = stack.top().GetName();
                PdfObject* font = page->GetFromResources("Font", reference_name);
                if (font) ans.insert(font->Reference());
            }
        }
    }
    return true;
}

static PyObject*
convert_w_array(const PdfArray &w) {
    pyunique_ptr ans(PyList_New(0));
    if (!ans) return NULL;
    for (PdfArray::const_iterator it = w.begin(); it != w.end(); it++) {
        pyunique_ptr item;
        if ((*it).IsArray()) {
            item.reset(convert_w_array((*it).GetArray()));
        } else if ((*it).IsNumber()) {
            item.reset(PyLong_FromLongLong((long long)(*it).GetNumber()));
        } else if ((*it).IsReal()) {
            item.reset(PyFloat_FromDouble((*it).GetReal()));
        } else PyErr_SetString(PyExc_ValueError, "Unknown datatype in w array");
        if (!item) return NULL;
        if (PyList_Append(ans.get(), item.get()) != 0) return NULL;
    }
    return ans.release();
}

#if PY_MAJOR_VERSION > 2
#define py_as_long_long PyLong_AsLongLong
#else
static inline long long
py_as_long_long(const PyObject *x) {
    if (PyInt_Check(x)) return PyInt_AS_LONG(x);
    return PyLong_AsLongLong(x);
}
#endif

static void
convert_w_array(PyObject *src, PdfArray &dest) {
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(src); i++) {
        PyObject *item = PyList_GET_ITEM(src, i);
        if (PyFloat_Check(item)) {
            dest.push_back(PdfObject(PyFloat_AS_DOUBLE(item)));
        } else if (PyList_Check(item)) {
            PdfArray sub;
            convert_w_array(item, sub);
            dest.push_back(sub);
        } else {
            pdf_int64 val = py_as_long_long(item);
            if (val == -1 && PyErr_Occurred()) { PyErr_Print(); continue; }
            dest.push_back(PdfObject(val));
        }
    }
}

extern "C" {
PyObject*
list_fonts(PDFDoc *self, PyObject *args) {
    int get_font_data = 0;
    if (!PyArg_ParseTuple(args, "|i", &get_font_data)) return NULL;
    pyunique_ptr ans(PyList_New(0));
    if (!ans) return NULL;
    try {
        const PdfVecObjects &objects = self->doc->GetObjects();
        for (TCIVecObjects it = objects.begin(); it != objects.end(); it++) {
            if ((*it)->IsDictionary()) {
                const PdfDictionary &dict = (*it)->GetDictionary();
                if (dictionary_has_key_name(dict, PdfName::KeyType, "Font") && dict.HasKey("BaseFont")) {
                    const std::string &name = dict.GetKey("BaseFont")->GetName().GetName();
                    const std::string &subtype = dict.GetKey(PdfName::KeySubtype)->GetName().GetName();
                    const PdfReference &ref = (*it)->Reference();
                    unsigned long num = ref.ObjectNumber(), generation = ref.GenerationNumber();
                    const PdfObject *descriptor = (*it)->GetIndirectKey("FontDescriptor");
                    pyunique_ptr descendant_font, stream_ref, encoding, w, w2;
                    PyBytesOutputStream stream_data, to_unicode;
                    if (dict.HasKey("W")) {
                        w.reset(convert_w_array(dict.GetKey("W")->GetArray()));
                        if (!w) return NULL;
                    }
                    if (dict.HasKey("W2")) {
                        w2.reset(convert_w_array(dict.GetKey("W2")->GetArray()));
                        if (!w2) return NULL;
                    }
                    if (dict.HasKey("Encoding") && dict.GetKey("Encoding")->IsName()) {
                        encoding.reset(PyUnicode_FromString(dict.GetKey("Encoding")->GetName().GetName().c_str()));
                        if (!encoding) return NULL;
                    }
                    if (descriptor) {
                        const PdfObject *ff = get_font_file(descriptor);
                        if (ff) {
                            stream_ref.reset(ref_as_tuple(ff->Reference()));
                            if (!stream_ref) return NULL;
                            const PdfStream *stream = ff->GetStream();
                            if (stream && get_font_data) {
                                stream->GetFilteredCopy(&stream_data);
                            }
                        }
                    } else if (dict.HasKey("DescendantFonts")) {
                        const PdfArray &df = dict.GetKey("DescendantFonts")->GetArray();
                        descendant_font.reset(ref_as_tuple(df[0].GetReference()));
                        if (!descendant_font) return NULL;
                        if (get_font_data && dict.HasKey("ToUnicode")) {
                            const PdfReference &uref = dict.GetKey("ToUnicode")->GetReference();
                            PdfObject *t = objects.GetObject(uref);
                            if (t) {
                                PdfStream *stream = t->GetStream();
                                if (stream) stream->GetFilteredCopy(&to_unicode);
                            }
                        }
                    }
#define V(x) (x ? x.get() : Py_None)
                    pyunique_ptr d(Py_BuildValue(
                            "{ss ss s(kk) sO sO sO sO sO sO sO}",
                            "BaseFont", name.c_str(),
                            "Subtype", subtype.c_str(),
                            "Reference", num, generation,
                            "Data", V(stream_data),
                            "DescendantFont", V(descendant_font),
                            "StreamRef", V(stream_ref),
                            "Encoding", V(encoding),
                            "ToUnicode", V(to_unicode),
                            "W", V(w), "W2", V(w2)
                    ));
#undef V
                    if (!d) { return NULL; }
                    if (PyList_Append(ans.get(), d.get()) != 0) return NULL;
                }
            }
        }
    } catch (const PdfError &err) {
        podofo_set_exception(err);
        return NULL;
    }
    return ans.release();
}

PyObject*
remove_fonts(PDFDoc *self, PyObject *args) {
    PyObject *fonts;
    if (!PyArg_ParseTuple(args, "O!", &PyTuple_Type, &fonts)) return NULL;
    PdfVecObjects &objects = self->doc->GetObjects();
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(fonts); i++) {
        unsigned long num, gen;
        if (!PyArg_ParseTuple(PyTuple_GET_ITEM(fonts, i), "kk", &num, &gen)) return NULL;
        PdfReference ref(num, gen);
        PdfObject *font = objects.GetObject(ref);
        if (font) {
            remove_font(objects, font);
        }
    }
    Py_RETURN_NONE;
}

typedef std::unordered_map<PdfReference, unsigned long, PdfReferenceHasher> charprocs_usage_map;

PyObject*
remove_unused_fonts(PDFDoc *self, PyObject *args) {
    unsigned long count = 0;
    try {
        unordered_reference_set used_fonts;
        for (int i = 0; i < self->doc->GetPageCount(); i++) {
            PdfPage *page = self->doc->GetPage(i);
            if (page) used_fonts_in_page(page, used_fonts);
        }
        unordered_reference_set all_fonts;
        unordered_reference_set type3_fonts;
        charprocs_usage_map charprocs_usage;
        PdfVecObjects &objects = self->doc->GetObjects();
        for (TCIVecObjects it = objects.begin(); it != objects.end(); it++) {
            if ((*it)->IsDictionary()) {
                const PdfDictionary &dict = (*it)->GetDictionary();
                if (dictionary_has_key_name(dict, PdfName::KeyType, "Font")) {
                    const std::string &font_type = dict.GetKey(PdfName::KeySubtype)->GetName().GetName();
                    if (font_type == "Type0") {
                        all_fonts.insert((*it)->Reference());
                    } else if (font_type == "Type3") {
                        all_fonts.insert((*it)->Reference());
                        type3_fonts.insert((*it)->Reference());
                        for (auto &x : dict.GetKey("CharProcs")->GetDictionary().GetKeys()) {
                            const PdfReference &ref = x.second->GetReference();
                            if (charprocs_usage.find(ref) == charprocs_usage.end()) charprocs_usage[ref] = 1;
                            else charprocs_usage[ref] += 1;
                        }
                    }
                }
            }
        }

        for (auto &ref : all_fonts) {
            if (used_fonts.find(ref) == used_fonts.end()) {
                PdfObject *font = objects.GetObject(ref);
                if (font) {
                    count++;
                    if (type3_fonts.find(ref) != type3_fonts.end()) {
                        for (auto &x : font->GetIndirectKey("CharProcs")->GetDictionary().GetKeys()) {
                            charprocs_usage[x.second->GetReference()] -= 1;
                        }
                    } else {
                        for (auto &x : font->GetIndirectKey("DescendantFonts")->GetArray()) {
                            PdfObject *dfont = objects.GetObject(x.GetReference());
                            if (dfont) remove_font(objects, dfont);
                        }
                    }
                    remove_font(objects, font);
                }
            }
        }

        for (auto &x : charprocs_usage) {
            if (x.second == 0u) {
                delete objects.RemoveObject(x.first);
            }
        }
    } catch(const PdfError &err) { podofo_set_exception(err); return NULL; }

    return Py_BuildValue("k", count);
}

PyObject*
merge_fonts(PDFDoc *self, PyObject *args) {
    PyObject *items, *replacements;
    if (!PyArg_ParseTuple(args, "O!O!", &PyTuple_Type, &items, &PyDict_Type, &replacements)) return NULL;
    std::unordered_map<uint64_t, uint64_t> ref_map;
    PdfVecObjects &objects = self->doc->GetObjects();
    PyObject *key, *value;
    Py_ssize_t pos = 0;
    size_t c = 0;
    while (PyDict_Next(replacements, &pos, &key, &value)) {
        c++;
        unsigned long num, gen;
        if (!PyArg_ParseTuple(key, "kk", &num, &gen)) return NULL;
        uint64_t k = ref_as_integer(num, gen);
        PdfReference ref(num, gen);
        PdfObject *font = objects.GetObject(ref);
        if (font) remove_font(objects, font);
        if (!PyArg_ParseTuple(value, "kk", &num, &gen)) return NULL;
        uint64_t v = ref_as_integer(num, gen);
        ref_map[k] = v;
    }
    if (c > 0) replace_font_references(self, ref_map);

    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(items); i++) {
        long num, gen, t0num, t0gen;
        PyObject *W, *W2;
        const char *data, *tounicode_data;
        Py_ssize_t sz, tounicode_sz;
        if (!PyArg_ParseTuple(PyTuple_GET_ITEM(items, i), "(ll)(ll)O!O!s#s#", &num, &gen, &t0num, &t0gen, &PyList_Type, &W, &PyList_Type, &W2, &data, &sz, &tounicode_data, &tounicode_sz)) return NULL;
        PdfReference ref(num, gen);
        PdfObject *font = objects.GetObject(ref);
        if (font) {
            if (PyObject_IsTrue(W)) {
                PdfArray w;
                convert_w_array(W, w);
                font->GetDictionary().AddKey("W", w);
            }
            if (PyObject_IsTrue(W2)) {
                PdfArray w;
                convert_w_array(W2, w);
                font->GetDictionary().AddKey("W2", w);
            }
            const PdfObject *descriptor = font->GetIndirectKey("FontDescriptor");
            if (descriptor) {
                PdfObject *ff = get_font_file(descriptor);
                PdfStream *stream = ff->GetStream();
                stream->Set(data, sz);
            }
        }
        if (tounicode_sz) {
            PdfObject *t0font = objects.GetObject(PdfReference(t0num, t0gen));
            if (t0font) {
                PdfObject *s = t0font->GetIndirectKey("ToUnicode");
                if (!s) { PyErr_SetString(PyExc_ValueError, "Type0 font has no ToUnicode stream"); return NULL; }
                s->GetStream()->Set(tounicode_data, tounicode_sz);
            }
        }
    }
    Py_RETURN_NONE;
}

}
