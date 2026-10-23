#ifndef ENGINE_SOURCE_RETAIL_SOURCECONTEXT_H
#define ENGINE_SOURCE_RETAIL_SOURCECONTEXT_H

// Virtual names the Retail model adapter gives to files it references by FileDataID
// ("retail/<id>.blp"). LocalSource resolves them through the listfile.
namespace Retail {

inline const char* SourcePrefix() { return "retail/"; }

inline bool IsVirtualPath(const char* name) {
    if (!name) return false;
    const char* prefix = SourcePrefix();
    for (; *prefix; ++prefix, ++name) {
        char c = *name;
        if (!c) return false;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c == '\\') c = '/';
        if (c != *prefix) return false;
    }
    return true;
}

}
#endif
