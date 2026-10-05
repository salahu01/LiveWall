import type { CSSProperties } from 'react';
import Effects from '@/components/Effects';
import { asset } from '@/lib/site';

export default function Home() {
  return (
    <>
      <div id="loader"><div className="bar"></div><div className="n mono"><span id="ln">000</span></div><div className="lbl">Decoding exactly one frame<br />at a time —<br />nothing more.</div></div>
      <div className="cur"></div><div className="cur-dot"></div>
      <div id="progress"></div>

      <nav>
        <a className="logo" href="#hero"><img src={asset('icon.png')} alt="" />LiveWall</a>
        <ul>
          <li><a href="#demo">Demo</a></li>
          <li><a href="#decisions">Design</a></li>
          <li><a href="#cost">Numbers</a></li>
          <li><a href="#platforms">Platforms</a></li>
          <li><a href="#get">Get it</a></li>
        </ul>
        <a className="logo mono" style={{fontSize: '12px', fontWeight: '400', letterSpacing: '.12em'}} href="https://github.com/salahu01/livewall-app">GITHUB ↗</a>
      </nav>

      {/* HERO */}
      <section id="hero">
        <canvas id="field"></canvas>
        <div className="veil"></div>
        <div className="content wrap">
          <div className="kicker hero-in">macOS · Windows · Linux · Android — MIT</div>
          <h1 className="title" aria-label="LiveWall"><span id="htitle">LiveWall</span></h1>
          <p className="hero-sub hero-in">A live wallpaper built around one constraint:<br /><span className="serif" style={{fontSize: '1.15em'}}>it should cost almost nothing when you aren't looking at it.</span></p>
          <div className="hero-row hero-in">
            <a className="btn solid magnet" href="#get">Download <span className="arr">→</span></a>
            <a className="btn magnet" href="#demo">Cover the desktop <span className="arr">↓</span></a>
          </div>
        </div>
        <div className="hero-hud hero-in">
          <div>FIELD <b>PROCEDURAL</b></div>
          <div>DECODER <b id="hudDec">ACTIVE</b></div>
          <div>FRAMES IN FLIGHT <b>1</b></div>
          <div>T <b id="hudT">00:00.000</b></div>
        </div>
        <div className="scrollhint">Scroll<i></i></div>
      </section>

      <div className="marquee"><div className="track" id="mq">
        <div className="it"><b>504 KB</b><span>macOS binary</span></div><div className="it"><em>✦</em></div>
        <div className="it"><b>12 MB</b><span>idle memory</span></div><div className="it"><em>✦</em></div>
        <div className="it"><b>0.0%</b><span>CPU when covered</span></div><div className="it"><em>✦</em></div>
        <div className="it"><b>2.9%</b><span>of one core, 4K 10-bit HEVC</span></div><div className="it"><em>✦</em></div>
        <div className="it"><b>102 KB</b><span>Android APK</span></div><div className="it"><em>✦</em></div>
        <div className="it"><b>459 KB</b><span>Linux daemon</span></div><div className="it"><em>✦</em></div>
        <div className="it"><b>0.991</b><span>SSIM across the loop seam</span></div><div className="it"><em>✦</em></div>
        <div className="it"><b>0</b><span>dependencies</span></div><div className="it"><em>✦</em></div>
      </div></div>

      {/* MANIFESTO */}
      <section id="manifesto" className="sec">
        <div className="wrap">
          <div className="sec-h"><div className="idx">00 / WHY</div><div></div></div>
          <p className="big" id="mtext">Most video wallpaper apps wrap a media player around whatever file you drop in, and leave it decoding whether or not the desktop is visible. That is why they show up in Activity Monitor and Task Manager, and why laptop fans spin. LiveWall takes the opposite approach — every design decision trades features for resources.</p>
          <div className="vs">
            <div className="vcard"><h4>Typical player · desktop covered</h4><div className="meter" id="mBad"></div><div className="cap"><span className="mono" style={{color: 'var(--dim)'}}>still decoding ~30 frames ahead</span><b style={{color: 'var(--a4)'}}>busy</b></div></div>
            <div className="vcard good"><h4>LiveWall · desktop covered</h4><div className="meter" id="mGood"></div><div className="cap"><span className="mono" style={{color: 'var(--dim)'}}>decoder destroyed, last frame composited</span><b style={{color: 'var(--a1)'}}>0.0%</b></div></div>
          </div>
        </div>
      </section>

      {/* DEMO */}
      <section id="demo" className="sec">
        <div className="wrap">
          <div className="sec-h">
            <div className="idx">01 / TRY IT</div>
            <div><h2 className="split">Drag a window over the wallpaper.</h2>
            <p>This is the coverage gate, running live. The desktop is sampled on the same 64×40 grid the apps use. Leave less than <b style={{color: 'var(--ink)'}}>8%</b> uncovered and the decoder is torn down after 0.4 s. It only comes back above <b style={{color: 'var(--ink)'}}>15%</b>. The gap stops a window edge sitting on the line from flipping the decoder on and off.</p></div>
          </div>
          <div className="demo-grid">
            <div className="desk" id="desk">
              <video id="deskVid" src={asset('media/aurora.mp4')} poster={asset('media/aurora-poster.jpg')} muted loop playsInline autoPlay preload="auto"></video>
              <canvas className="grid" id="gridCv"></canvas>
              <div className="menubar"><span> Finder&nbsp;&nbsp;File&nbsp;&nbsp;Edit&nbsp;&nbsp;View</span><span><img src={asset('icon.png')} alt="" style={{filter: 'none', borderRadius: '3px'}} /> &nbsp;<span id="clock">9:41</span></span></div>
              <div className="win" id="w1" style={{left: '6%', top: '14%', width: '44%', height: '52%'}}><div className="tb"><i></i><i></i><i></i><span>Terminal — zsh</span></div><div className="body"><div>$ swift test</div><div style={{color: 'var(--a1)'}}>✔ 31 tests passed</div><div>$ ./tools/measure.sh 20</div><div>mem 12 MB · cpu 0.0%</div><div className="l" style={{width: '70%'}}></div><div className="l" style={{width: '40%'}}></div></div><div className="rs"></div></div>
              <div className="win" id="w2" style={{left: '46%', top: '30%', width: '46%', height: '58%'}}><div className="tb"><i></i><i></i><i></i><span>Safari — Docs</span></div><div className="body"><div className="l" style={{width: '90%', height: '14px'}}></div><div className="l" style={{width: '60%'}}></div><div className="l" style={{width: '80%'}}></div><div className="l" style={{width: '70%'}}></div><div className="l" style={{width: '85%'}}></div><div className="l" style={{width: '50%'}}></div></div><div className="rs"></div></div>
              <div className="badge"><i></i><span id="badgeTxt">DECODING · 24 fps</span></div>
            </div>
            <div className="panel">
              <div><h5>Decoder</h5><div className="state" id="state"><i></i><span>Playing</span></div></div>
              <div>
                <h5>Desktop uncovered</h5>
                <div className="gauge" style={{marginTop: '12px'}}><div className="fill" id="gfill"></div><div className="th" style={{left: '8%'}}></div><div className="th" style={{left: '15%', background: 'var(--a1)'}}></div></div>
                <div className="gauge-l"><span id="gval">100%</span><span>stop &lt; 8% · resume &gt; 15%</span></div>
              </div>
              <div className="readout">
                <div><small>CPU · 1 core</small><b id="rcpu">2.9%</b></div>
                <div><small>Memory</small><b id="rmem">19 MB</b></div>
              </div>
              <svg className="spark" id="spark" viewBox="0 0 300 46" preserveAspectRatio="none"><path id="sparkP" fill="none" stroke="#5ef2c4" strokeWidth="1.5"/></svg>
              <div className="toggles">
                <button className="chip" id="tGrid">Show 64×40 grid</button>
                <button className="chip" id="tMax">Maximise</button>
                <button className="chip" id="tReset">Reset</button>
              </div>
              <div><h5>Gate log</h5><div className="log" id="log"></div></div>
            </div>
          </div>
        </div>
      </section>

      {/* DECISIONS */}
      <section id="decisions">
        <div className="hwrap">
          <div className="htrack" id="htrack">
            <div className="dintro">
              <div className="kicker">02 / The design</div>
              <h2 style={{marginTop: '20px'}}>Five<br /><span className="serif grad">decisions.</span></h2>
              <p>All four apps are one program written in four languages. These five decisions are what keep them one project.</p>
            </div>
            <article className="dcard"><div className="num">01 — TEARDOWN</div><h3>Visibility tears down. It does not pause.</h3><p>When nothing can see the wallpaper, the decoder and its session are <b style={{color: 'var(--ink)'}}>destroyed</b>. The window server keeps showing the last frame for free, and playback resumes from the saved timestamp.</p><div className="viz"><canvas data-viz="teardown"></canvas></div></article>
            <article className="dcard"><div className="num">02 — COVERAGE</div><h3>Partial coverage counts.</h3><p>The OS reports a desktop with one visible corner as "visible". LiveWall measures the uncovered fraction on a 64×40 grid instead. It stops below 8% and resumes above 15%.</p><div className="viz"><canvas data-viz="coverage"></canvas></div></article>
            <article className="dcard"><div className="num">03 — ONE FRAME</div><h3>One frame in flight.</h3><p>Each tick pulls one decoded frame and hands it straight to the compositor. There's no read-ahead queue and no media clock. At 4K 10-bit, one frame is ~20 MB, so a read-ahead queue gets expensive fast.</p><div className="viz"><canvas data-viz="oneframe"></canvas></div></article>
            <article className="dcard"><div className="num">04 — NATIVE FORMAT</div><h3>Never ask for a format the decoder isn't producing.</h3><p>Naming a pixel format costs a kernel round trip per frame. Not naming one cut <span className="mono">copyNextSampleBuffer</span> from <b style={{color: 'var(--ink)'}}>65 profiler samples to 4</b>. LiveWall takes the decoder's native 4:2:0 output and converts it in the compositor, where it's free.</p><div className="viz"><canvas data-viz="format"></canvas></div></article>
            <article className="dcard"><div className="num">05 — IMPORT</div><h3>Import is mandatory.</h3><p>Every video is converted on import, and only the converted file is ever played. The playback path never sees an unknown codec, an oversized frame, 60 fps, an audio track or a B-frame. Your original file is never touched.</p><div className="viz"><canvas data-viz="import"></canvas></div></article>
          </div>
        </div>
      </section>

      {/* COST */}
      <section id="cost" className="sec">
        <div className="wrap">
          <div className="sec-h">
            <div className="idx">03 / MEASURED</div>
            <div><h2 className="split">Pixels are free. Bits are free. <span className="grad">Frames are expensive.</span></h2>
            <p>Measured on an M3 Pro against the real playback path. Same clip each time, one variable changed per row. Hardware decode costs the same at any frame size. What's left is the app's own per-frame work, and that grows with frames per second and nothing else.</p></div>
          </div>
          <div className="chart" id="chart"></div>
          <div className="chart-legend"><span><i></i>CPU, % of one core</span><span><u></u>Pixels relative to 720p</span></div>
          <div className="triptych">
            <div><b className="count" data-to="7.4" data-dec="1" data-suf="×">0</b><span>more pixels → +0.4% CPU</span></div>
            <div><b className="count" data-to="2.5" data-dec="1" data-suf="×">0</b><span>the frame rate (24 → 60 fps) → 7.78% CPU</span></div>
            <div><b className="count" data-to="28" data-suf=" MB">0</b><span>AVPlayer-style read-ahead vs 19 MB</span></div>
          </div>
        </div>
      </section>

      {/* PIPELINE */}
      <section id="pipeline" className="sec" style={{paddingTop: '0'}}>
        <div className="wrap">
          <div className="sec-h">
            <div className="idx">04 / IN FLIGHT</div>
            <div><h2 className="split">The queue nobody needed.</h2>
            <p>Both pipelines decode the same clip. The top one buffers ahead like a media player does. The bottom one is LiveWall: one tick, one sample, shown immediately. Its buffer pool stays at the minimum.</p></div>
          </div>
          <div className="lanes">
            <div className="lane"><header><b>Media player</b><span className="mono">read-ahead ≈ 30 frames · <span id="pq1">0</span> MB resident</span></header><canvas id="lane1"></canvas></div>
            <div className="lane"><header><b>LiveWall</b><span className="mono">1 frame in flight · <span id="pq2">0</span> MB resident</span></header><canvas id="lane2"></canvas></div>
          </div>
        </div>
      </section>

      {/* GATES */}
      <section id="gates" className="sec" style={{background: 'var(--bg2)', borderTop: '1px solid var(--line)', borderBottom: '1px solid var(--line)'}}>
        <div className="wrap">
          <div className="sec-h">
            <div className="idx">05 / STOP SIGNALS</div>
            <div><h2 className="split">Hard stops, not slowdowns.</h2>
            <p>The assets have no B-frames, so every frame must be decoded whether it's shown or not. Lowering the tick rate would just play the clip in slow motion. So any one of these conditions stops playback, and the last frame stays on screen. Tap a card to trigger it.</p></div>
          </div>
          <div className="gates" id="gatesEl"></div>
          <div className="gate-out"><span className="pill" id="gpill">all clear</span><span>decoder →</span><b id="gstate">running</b></div>
        </div>
      </section>

      {/* IMPORT */}
      <section id="import" className="sec">
        <div className="wrap">
          <div className="sec-h">
            <div className="idx">06 / IMPORT</div>
            <div><h2 className="split">Unknown files are fixed once, at import.</h2>
            <p>Each problem below would cost something on every frame if the file were played as-is. Converting at import fixes it once. The conversion uses the system encoder, with no bundled ffmpeg. Bundling it would add 40–70 MB, plus a licensing question, to a 504 KB app.</p></div>
          </div>
          <div className="imp" id="imp">
            <div className="row head"><div>Source problem · cost if played directly</div><div>Fixed at import</div></div>
            <div className="row"><div><s>VP9 / AV1 / ProRes</s>&nbsp;software decode, tens of % CPU</div><div><span className="ok">✓</span>Transcoded to HEVC → media engine</div></div>
            <div className="row"><div><s>Larger than the panel</s>&nbsp;decodes pixels you can't see</div><div><span className="ok">✓</span>Scaled to cover the display, never past 1:1</div></div>
            <div className="row"><div><s>60 fps</s>&nbsp;2.5× the pump work</div><div><span className="ok">✓</span>Capped, snapped to a divisor of the refresh rate</div></div>
            <div className="row"><div><s>Audio track</s>&nbsp;spins up an audio graph</div><div><span className="ok">✓</span>Stripped</div></div>
            <div className="row"><div><s>B-frames</s>&nbsp;decoder needs a reorder buffer</div><div><span className="ok">✓</span>Frame reordering disabled</div></div>
            <div className="row"><div><s>moov atom at EOF</s>&nbsp;full-file read before playback</div><div><span className="ok">✓</span>Optimised for streaming</div></div>
          </div>
          <div className="presets">
            <div className="preset"><small>Preset</small><h6>Ultra Light</h6><dl><dt>size</dt><dd>960p</dd><dt>rate</dt><dd>20 fps</dd><dt>depth</dt><dd>8-bit</dd></dl></div>
            <div className="preset"><small>Preset</small><h6>Balanced</h6><dl><dt>size</dt><dd>1920p</dd><dt>rate</dt><dd>24 fps</dd><dt>depth</dt><dd>8-bit</dd></dl></div>
            <div className="preset def"><small>Default for new imports</small><h6>Native</h6><dl><dt>size</dt><dd>your display</dd><dt>rate</dt><dd>24 fps</dd><dt>depth</dt><dd>10-bit</dd></dl></div>
          </div>
        </div>
      </section>

      {/* PLATFORMS */}
      <section id="platforms" className="sec" style={{paddingTop: '0'}}>
        <div className="wrap">
          <div className="sec-h">
            <div className="idx">07 / PLATFORMS</div>
            <div><h2 className="split">One design, four native ports.</h2>
            <p>Each port uses its platform's own APIs: no cross-platform runtime, no web view, and nothing that runs in the background without a reason. Each README covers how the port stays cheap, what was measured, and what it gives up.</p></div>
          </div>
          <div className="plats">
            <div className="plat tilt" style={{'--c': '#5ef2c4'} as CSSProperties}><div className="glow"></div><div className="os">Menu bar</div><h3>macOS</h3><div className="stack">Swift · AppKit · AVFoundation · Metal</div><ul><li>Sits at <span className="mono">kCGDesktopWindowLevel</span></li><li>Gradient mode in CAGradientLayer, costing the app nothing</li><li>Thermal + Low Power gates</li></ul><div className="hero-n"><b>504<small style={{fontSize: '.4em'}}> KB</small></b><span>binary · 12 MB idle</span></div><a className="dl" href="https://github.com/salahu01/livewall-app/releases/latest/download/LiveWall-macos.zip">LiveWall-macos.zip <span>↓</span></a></div>
            <div className="plat tilt" style={{'--c': '#7c6bff'} as CSSProperties}><div className="glow"></div><div className="os">Notification area</div><h3>Windows</h3><div className="stack">C++20 · Win32 · Direct3D 11 · Media Foundation</div><ul><li><span className="mono">WorkerW</span> parenting, with a fallback</li><li>HEVC Main10 → Main → H.264 ladder</li><li>No admin rights; writes only to HKCU</li></ul><div className="hero-n"><b>45</b><span>tests · no GPU required</span></div><a className="dl" href="https://github.com/salahu01/livewall-app/releases/download/windows-v1.0.4/LiveWall.exe">LiveWall.exe · 1.0.4 <span>↓</span></a></div>
            <div className="plat tilt" style={{'--c': '#ff8a4c'} as CSSProperties}><div className="glow"></div><div className="os">Daemon</div><h3>Linux</h3><div className="stack">C++ · X11 + Wayland · EGL · dlopen'd FFmpeg/VA-API</div><ul><li>X11 backend measures coverage, even under Wayland</li><li>wlr-layer-shell for sway, Hyprland, KWin</li><li>Optional libraries loaded only at runtime</li></ul><div className="hero-n"><b>459<small style={{fontSize: '.4em'}}> KB</small></b><span>binary · ldd checked in CI</span></div><a className="dl" href="https://github.com/salahu01/livewall-app/releases/latest/download/livewall">livewall <span>↓</span></a></div>
            <div className="plat tilt" style={{'--c': '#ff4d8d'} as CSSProperties}><div className="glow"></div><div className="os">Live wallpaper</div><h3>Android</h3><div className="stack">Kotlin · WallpaperService · MediaCodec · GLES</div><ul><li><span className="mono">onVisibilityChanged</span> is the gate</li><li>Service is 280 lines, comments included</li><li>No AndroidX, no Compose, no media3</li></ul><div className="hero-n"><b>102<small style={{fontSize: '.4em'}}> KB</small></b><span>release APK · 0.00% hidden</span></div><a className="dl" href="https://github.com/salahu01/livewall-app/releases/latest/download/LiveWall-android.apk">LiveWall-android.apk <span>↓</span></a></div>
          </div>

          <div className="diff">
            <div className="tabs" id="dtabs"></div>
            <div className="pane"><div><small>macOS</small><p id="dmac"></p></div><div><small>Windows</small><p id="dwin"></p></div></div>
          </div>
        </div>
      </section>

      {/* SAMPLES */}
      <section id="samples" className="sec" style={{paddingTop: '0'}}>
        <div className="wrap">
          <div className="sec-h">
            <div className="idx">08 / SAMPLES</div>
            <div><h2 className="split">Two loops, made by the app.</h2>
            <p>Both clips come from LiveWall's own procedural field, not from stock footage. They're original work under the repo's MIT licence, and you can regenerate them with <span className="mono">tools/make-samples.sh</span>.</p></div>
          </div>
          <div className="samples">
            <div className="sample"><video src={asset('media/aurora.mp4')} poster={asset('media/aurora-poster.jpg')} muted loop playsInline preload="none" data-lazy></video><div className="cap"><b>aurora</b><span>1920×1080 · 24 fps · 10 s</span></div></div>
            <div className="sample"><video src={asset('media/ember.mp4')} poster={asset('media/ember-poster.jpg')} muted loop playsInline preload="none" data-lazy></video><div className="cap"><b>ember</b><span>1920×1080 · 24 fps · 10 s</span></div></div>
          </div>
          <div className="ssim"><b className="grad count" data-to="0.991" data-dec="3">0</b><p>SSIM measured across the loop seam. 1.0 would mean the last frame and the first frame are identical. You won't see the cut.</p></div>
        </div>
      </section>

      {/* GET */}
      <section id="get" className="sec" style={{paddingTop: '0'}}>
        <div className="wrap">
          <div className="sec-h">
            <div className="idx">09 / GET IT</div>
            <div><h2 className="split">Install it, then forget it's running.</h2>
            <p>Grab a build from <a href="https://github.com/salahu01/livewall-app/releases">Releases</a>, or build from source. Then pick <b style={{color: 'var(--ink)'}}>Add Video…</b> from the menu.</p></div>
          </div>
          <div className="term">
            <div className="tb"><i></i><i></i><i></i><div className="tt" id="ttabs"></div></div>
            <pre id="tout"></pre>
          </div>
        </div>
      </section>

      <footer>
        <div className="wrap">
          <div className="kicker">Open source · MIT</div>
          <div className="cta" style={{marginTop: '24px'}}><a href="https://github.com/salahu01/livewall-app" className="split2">Star it on<br /><span className="grad">GitHub ↗</span></a></div>
          <div className="bottom">
            <span>LiveWall — © salahu01 · MIT licence</span>
            <span><a href="https://github.com/salahu01/livewall-app">Source</a> · <a href="https://github.com/salahu01/livewall-app/releases">Releases</a> · <a href="https://github.com/salahu01/livewall-app/issues">Issues</a> · <a href="https://salahu01.github.io">salahu01.github.io</a></span>
          </div>
        </div>
      </footer>
      <Effects />
    </>
  );
}
