/* Public entitlement keys referenced by kern_exec.c and necp_client.c.
 * Values are also present in XNU tests/entitlements/hardened-binary-*.plist.
 */
#ifndef XNU_CODESIGNATURE_ENTITLEMENTS_H
#define XNU_CODESIGNATURE_ENTITLEMENTS_H
#define kCSWebBrowserHostEntitlement "com.apple.developer.web-browser-engine.host"
#define kCSWebBrowserGPUEntitlement "com.apple.developer.web-browser-engine.rendering"
#define kCSWebBrowserNetworkEntitlement "com.apple.developer.web-browser-engine.networking"
#define kCSWebBrowserWebContentEntitlement "com.apple.developer.web-browser-engine.webcontent"
#endif
