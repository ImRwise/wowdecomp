#ifndef WOW_COMMON_TEXTURECACHE_H
#define WOW_COMMON_TEXTURECACHE_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/BLPFile/Types.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "WoW/Common/ComponentCore/Types.h"

class CACHEENTRY;
class CAsyncObject;

class CACHEENTRY : public TSHashObject<CACHEENTRY, HASHKEY_NONE> {
    public:
    /* 0018 */ CAsyncObject* m_asyncObject;
    /* 001C */ TCTEXTUREINFO m_info;
    /* 0024 */ char m_fileName [128];
    /* 00A4 */ int32_t m_refCount;
    /* 00A8 */ uint32_t m_memHandle;
    /* 00AC */ BLPHeader* m_data;
    // Esteria: was a 20-bit field, so any BLP over 1 MB (a 1024x1024 DXT5 armor texture is 1,049,748 bytes) was
    // allocated truncated while the async read still wrote the whole file into it. Keep the full 32-bit size.
    /* 00B0 */ uint32_t m_size;
    /* 00B4 */ uint32_t m_loadFailed : 1;
    /* 00B4 */ uint32_t m_unused : 31;

    CACHEENTRY();
    ~CACHEENTRY();
    int32_t LoadTexture();
};

void TextureCacheComputeLoadProgress(void* handle, int64_t& local, int64_t& total);
void* TextureCacheCopyTexture(void* handle);
void* TextureCacheCreateTexture(const char* fileName);
void TextureCacheDestroy();
void TextureCacheDestroyTexture(void* handle);
int32_t TextureCacheGetInfo(void* handle, TCTEXTUREINFO& info, int32_t wait);
void* TextureCacheGetMip(void* handle, uint32_t mipLevel);
BlpPalPixel* TextureCacheGetPal(void* handle);
// The whole loaded BLP file (any encoding) and its true byte size, or false while loading / on failure.
bool TextureCacheGetFile(void* handle, const uint8_t*& data, uint32_t& size);
const char* TextureCacheGetName(void* handle);
int32_t TextureCacheHasMips(void* handle);
void TextureCacheResetLoadCount();

#endif
