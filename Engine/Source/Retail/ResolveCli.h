#ifndef ENGINE_SOURCE_RETAIL_RESOLVECLI_H
#define ENGINE_SOURCE_RETAIL_RESOLVECLI_H
namespace Retail::Resolve {
// RetailAssetProbe <RetailCasc.ini|cdn_config.json> --resolve-items <list> [--out file] [options]
int ResolveMain(int argc, char** argv);
}
#endif
