#include "scalar/ScalarVoice.h"
#include "param_bridge.h"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <cstddef>
#include <thread>
#include <atomic>
#include <bit>
#include <limits>

struct alignas(std::max_align_t) Allocation { size_t size; };
static size_t liveBytes=0, peakBytes=0, allocations=0;
void* operator new(size_t n) {
    auto* h = static_cast<Allocation*>(std::malloc(sizeof(Allocation)+n));
    if (!h) throw std::bad_alloc();
    h->size=n; liveBytes+=n; peakBytes=std::max(peakBytes,liveBytes); ++allocations;
    return h+1;
}
void operator delete(void* p) noexcept { if (p) { auto* h=static_cast<Allocation*>(p)-1; liveBytes-=h->size; std::free(h); } }
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, size_t) noexcept { ::operator delete(p); }

template<class T> void memory(const char* label) {
    size_t baseline=liveBytes; peakBytes=liveBytes;
    { T obj; obj.prepare(48000); std::printf("memory %s sizeof=%zu heap_retained=%zu peak_delta=%zu\n",label,sizeof(T),liveBytes-baseline,peakBytes-baseline); }
}
float peak(scalar::ScalarVoice& v, int samples) { float result=0; for(int i=0;i<samples;++i) { auto [l,r]=v.processSample(); result=std::max({result,std::abs(l),std::abs(r)}); } return result; }
int main() {
    {
        scalar::ParamBridge b; scalar::VoiceParams out;
        b.staging().level=.1f; b.publish(); bool first=b.consume(out);
        b.staging().level=.2f; b.publish(); b.staging().level=.3f; b.publish(); bool last=b.consume(out);
        std::printf("bridge first=%d after_two_publishes=%d observed_level=%.2f expected=0.30\n",first,last,out.level);
    }
    memory<WavetableOsc>("wavetable"); memory<TerrainOsc>("terrain"); memory<GranularOsc>("granular"); memory<scalar::ScalarVoice>("voice_after_default_source_cached");
    std::printf("sizeof SingleFilter=%zu DualFilter=%zu VoiceParams=%zu SampleData=%zu\n",sizeof(SingleFilter),sizeof(DualFilter),sizeof(scalar::VoiceParams),sizeof(GranularOsc::SampleData));
    {
        scalar::ScalarVoice v; scalar::VoiceParams p; v.prepare(48000); p.ampEnv.attackSec=0; p.ampEnv.decaySec=0; p.ampEnv.sustain=1; v.setParams(p); v.noteOn(69,1);
        float before=peak(v,4096); p.engine=scalar::Engine::Wavetable; v.setParams(p); float after=peak(v,4096);
        std::printf("engine_switch peak_before=%g peak_after=%g active=%d\n",before,after,v.isActive());
    }
    {
        scalar::ScalarVoice v; scalar::VoiceParams p; v.prepare(48000); p.va.shape=VAOscillator::Shape::Sine; p.filterA.mix=0; p.ampEnv.attackSec=0; p.ampEnv.decaySec=0; p.ampEnv.sustain=1; v.setParams(p);
        constexpr float hz=432; v.setFrequency(hz); int note=int(69+12*std::log2(hz/440)+.5f); v.noteOn(note,1);
        peak(v,4800); int crossings=0; float prev=0; for(int i=0;i<48000;++i) { float s=v.processSample().first; if(prev<0 && s>=0) ++crossings; prev=s; }
        std::printf("continuous_pitch requested=%g derived_note=%d measured_hz=%d\n",hz,note,crossings);
    }
    {
        ADSREnvelope env; ADSREnvelope::Params p; p.attackSec=1; p.sustain=1; env.prepare(48000); env.setParams(p); env.noteOn(); int n=0; while(env.getStage()==ADSREnvelope::Stage::Attack && n<96000) {env.processSample();++n;}
        std::printf("adsr attack_requested=1.0 actual_sec=%.6f samples=%d\n",n/48000.,n);
        FMAlgorithm fm; fm.prepare(48000); for(int i=0;i<10;++i) fm.getOperator(i).getEnvelope().setParams(p); fm.noteOn(); n=0;
        while(fm.getOperator(0).getEnvelope().getStage()==ADSREnvelope::Stage::Attack && n<96000) { fm.processSample(); fm.advanceEnvelopes(); ++n; }
        std::printf("fm attack_requested=1.0 actual_sec=%.6f samples=%d\n",n/48000.,n);
    }
    {
        WavetableOsc osc; osc.prepare(48000); WavetableOsc::Params p; float a[1024]; osc.setParams(p); osc.reset(); for(float& x:a) x=osc.processSample().first;
        p.warpAmount=1; osc.setParams(p); osc.reset(); float diff=0; for(float x:a) diff=std::max(diff,std::abs(x-osc.processSample().first));
        std::printf("wavetable default_warpMode macro_amount_0_vs_1_max_diff=%g\n",diff);
    }
    {
        TerrainOsc osc; std::vector<float> grid(512*512, .1f); size_t base=liveBytes; peakBytes=liveBytes; osc.setHeightmap(grid.data(),512,512);
        std::printf("terrain512 heap_retained=%zu peak_delta=%zu (excludes input grid)\n",liveBytes-base,peakBytes-base);
    }
    for(int e=0;e<int(scalar::Engine::NumEngines);++e) {
        scalar::ScalarVoice v; scalar::VoiceParams p; p.engine=scalar::Engine(e); v.prepare(48000); v.setParams(p); v.noteOn(69,1); size_t count=allocations;
        float pk=peak(v,48000); std::printf("render %-7s allocations=%zu peak=%.5f\n",scalar::engineName(p.engine),allocations-count,pk);
    }
    int bad=0, cases=0; float maxPeak=0;
    for(int m=0;m<int(FilterMode::NumModes);++m) for(float cutoff:{20.f,1000.f,23520.f}) for(float res:{0.f,.5f,1.f}) for(int os:{0,1,2}) {
        SingleFilter f; f.prepare(48000); SingleFilter::Params p; p.mode=FilterMode(m); p.cutoff=cutoff; p.resonance=res; p.drive=1; p.oversample=os; p.saturation=1; p.morph=.75f; f.setParams(p); f.reset(); ++cases;
        for(int i=0;i<2048;++i) { float x=(i<1024)?std::sin(i*.1f):0.f; float y=f.processSample(x,1,.5f,.5f); if(!std::isfinite(y)) {++bad;break;} maxPeak=std::max(maxPeak,std::abs(y)); }
    }
    std::printf("filter_sweep cases=%d nonfinite_cases=%d max_peak=%g\n",cases,bad,maxPeak);
}
