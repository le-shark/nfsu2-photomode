#pragma once
// WidescreenFix's ConsoleGamma, run on a photo (user 2026-11-02). Photos used to be read back from the screen after
// WidescreenFix's EndScene work, which was the only way to get its gamma, and cut 2x photos into four screen pieces
// whose edges FSAA cleared. They are now resolved off screen, and the gamma comes from WidescreenFix itself: its own
// compiled effect, an RCDATA resource of its .asi (ID 201 in the installed build, found by content), loaded through
// the d3dx9_43.dll WidescreenFix imports, technique "ConsoleGamma" with the texture "InputTex2D", drawn as its
// RenderGamma and DrawScreenQuad do (refs/widescreenfix_nfsu2/postfxcore.ixx 305-351, 473-539). Whether it runs at all
// is WidescreenFix's own INI key [GRAPHICS] ConsoleGamma (default 1, PostFX.ixx:19).
#include <string>

struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;
struct IDirect3DStateBlock9;

namespace photomode::wsfgamma {

enum class Use {
  None,    // no WidescreenFix with its gamma effect in the process: the screen has no gamma either
  Off,     // WidescreenFix's INI has ConsoleGamma = 0
  On,      // loaded: Apply runs it
  Failed,  // ConsoleGamma is on, but its effect could not be loaded here (logged): photos without it
};

class Gamma {
 public:
  ~Gamma() { Release(); }
  Use Load(IDirect3DDevice9* dev);
  // The same from a given module (tools/gamma_check loads WidescreenFix's .asi as a data file); ini empty =
  // ConsoleGamma taken as on.
  Use LoadFrom(IDirect3DDevice9* dev, void* module, const std::string& name, const std::string& ini);
  bool Loaded() const { return effect_ != nullptr; }
  // dst is a render target of src's size; src is not bound anywhere. Everything the draw changes is put back.
  bool Apply(IDirect3DDevice9* dev, IDirect3DTexture9* src, IDirect3DSurface9* dst);
  void Release();  // before a device reset too (DEFAULT-pool resources)

 private:
  void* effect_ = nullptr;  // ID3DXEffect
  const char* tech_ = nullptr;   // D3DXHANDLE
  const char* input_ = nullptr;  // D3DXHANDLE
  IDirect3DStateBlock9* state_ = nullptr;
};

// The pure parts, for the tests: the INI next to a module, and whether an RCDATA blob is WidescreenFix's effect.
std::string IniPathFor(const std::string& modulePath);
bool IsGammaEffect(const void* data, unsigned long size);

}  // namespace photomode::wsfgamma
