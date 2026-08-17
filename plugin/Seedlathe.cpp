#include "Seedlathe.h"
#include "IPlug_include_in_plug_src.h"

Seedlathe::Seedlathe(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  GetParam(kParamVolume)->InitDouble("Volume", 100., 0., 500., 0.1, "%");

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS,
                        GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->AttachPanelBackground(IColor(255, 24, 26, 30));
    pGraphics->EnableMouseOver(true);

    const IRECT b = pGraphics->GetBounds().GetPadded(-16.f);
    pGraphics->AttachControl(new IVKeyboardControl(b.GetFromBottom(160.f)),
                             kCtrlTagKeyboard);
  };
#endif
}

#if IPLUG_DSP
void Seedlathe::OnReset()
{
  // Engine wiring lands in Task 16.
}

void Seedlathe::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  for (int c = 0; c < NOutChansConnected(); ++c)
    for (int s = 0; s < nFrames; ++s)
      outputs[c][s] = 0.;
}
#endif
