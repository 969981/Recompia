// Synthetic stand-in for MPR main: a few C++ symbols with the same shapes the KB talks about.
extern "C" void __cxa_pure_virtual() {}
struct CFinalInput { char d[0x398]; };
struct CStateManager { int x; };
struct STonemapParams { float inverseExposure; };

struct COsContext { static int GetScreenWidth(); static int GetScreenHeight(); };
__attribute__((optnone, noinline)) int COsContext::GetScreenWidth() { return 0x640; }
__attribute__((optnone, noinline)) int COsContext::GetScreenHeight() { return 0x384; }

struct CEntityMP1 {
  virtual ~CEntityMP1();
  virtual void Think(float, CStateManager&);
  int pad[4];
};
CEntityMP1::~CEntityMP1() {}
void CEntityMP1::Think(float, CStateManager&) {}

struct CPlayerMP1 : CEntityMP1 {
  ~CPlayerMP1() override;
  void Think(float, CStateManager&) override;
  virtual void Extra(int);
  char fill[0x5cc - 0x18];
  float pitch;                       // +0x5cc, like the real CPlayerMP1
  void ProcessInput(const CFinalInput&, CStateManager&);
};
CPlayerMP1::~CPlayerMP1() {}
void CPlayerMP1::Think(float, CStateManager&) {}
void CPlayerMP1::Extra(int) {}
volatile int g_sink;
void CPlayerMP1::ProcessInput(const CFinalInput& in, CStateManager& m) {
  pitch += 1.0f;                     // ldr s0,[x0,#0x5cc]
  g_sink = COsContext::GetScreenWidth() + COsContext::GetScreenHeight() + m.x + in.d[0];   // two bl sites
}
namespace NTonemap { void build_tonemap_eval_params(const STonemapParams& p) { g_sink = (int)p.inverseExposure; } }
extern "C" void nvnBootstrapLoader() {}
CPlayerMP1 g_player;                 // forces the vtable out
