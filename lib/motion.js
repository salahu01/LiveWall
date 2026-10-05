// Page motion and interactive demos, ported from the original inline script.
// Runs once per page load; it builds parts of the DOM (meters, chart rows,
// gate cards, tabs) imperatively, so it must not run twice.
import gsap from 'gsap';
import { ScrollTrigger } from 'gsap/ScrollTrigger';

export function startEffects() {
  const RM = matchMedia('(prefers-reduced-motion:reduce)').matches;
  const $=s=>document.querySelector(s), $$=s=>[...document.querySelectorAll(s)];
  gsap.registerPlugin(ScrollTrigger);
  
  /* ---------- cursor ---------- */
  const cur=$('.cur'),dot=$('.cur-dot');let mx=innerWidth/2,my=innerHeight/2,cx=mx,cy=my;
  addEventListener('pointermove',e=>{mx=e.clientX;my=e.clientY;dot.style.transform=`translate(${mx}px,${my}px)`});
  (function loop(){cx+=(mx-cx)*.18;cy+=(my-cy)*.18;cur.style.transform=`translate(${cx}px,${cy}px)`;requestAnimationFrame(loop)})();
  $$('a,button,.gate,.win,.chip').forEach(el=>{el.addEventListener('pointerenter',()=>cur.classList.add('big'));el.addEventListener('pointerleave',()=>cur.classList.remove('big'))});
  
  /* magnet buttons */
  $$('.magnet').forEach(b=>{b.addEventListener('pointermove',e=>{const r=b.getBoundingClientRect();gsap.to(b,{x:(e.clientX-r.left-r.width/2)*.3,y:(e.clientY-r.top-r.height/2)*.4,duration:.4})});b.addEventListener('pointerleave',()=>gsap.to(b,{x:0,y:0,duration:.7,ease:'elastic.out(1,.4)'}))});
  
  /* ---------- WebGL field ---------- */
  const cv=$('#field');const gl=cv.getContext('webgl',{antialias:false,premultipliedAlpha:false});
  let fieldOn=true, mouse=[.5,.5], smouse=[.5,.5];
  if(gl){
    const vs=`attribute vec2 p;void main(){gl_Position=vec4(p,0,1);}`;
    const fs=`precision highp float;uniform vec2 r;uniform float t;uniform vec2 m;uniform float sc;
    float h(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
    float n(vec2 p){vec2 i=floor(p),f=fract(p);f=f*f*(3.-2.*f);return mix(mix(h(i),h(i+vec2(1,0)),f.x),mix(h(i+vec2(0,1)),h(i+vec2(1,1)),f.x),f.y);}
    float fbm(vec2 p){float v=0.,a=.5;for(int i=0;i<5;i++){v+=a*n(p);p=p*2.02+vec2(1.7,9.2);a*=.5;}return v;}
    void main(){vec2 uv=gl_FragCoord.xy/r;vec2 p=uv*vec2(r.x/r.y,1.);
      float tt=t*.05;vec2 mm=(m-uv)*vec2(r.x/r.y,1.);float md=exp(-dot(mm,mm)*6.);
      vec2 q=vec2(fbm(p*1.6+tt),fbm(p*1.6-tt+3.1));
      vec2 w=vec2(fbm(p*1.3+2.*q+vec2(1.7,9.2)+tt*1.4+md*.6),fbm(p*1.3+2.*q+vec2(8.3,2.8)-tt));
      float f=fbm(p*1.1+2.4*w);
      vec3 c1=vec3(.03,.04,.09),c2=vec3(.37,.95,.77),c3=vec3(.49,.42,1.),c4=vec3(1.,.3,.55);
      vec3 col=mix(c1,c3,smoothstep(.2,.8,f));col=mix(col,c2,smoothstep(.45,.95,w.y)*.8);col=mix(col,c4,smoothstep(.55,1.,q.x)*.55);
      col*=.6+1.1*f; col+=md*.08*c2;
      float band=smoothstep(.0,.6,uv.y)*(1.-smoothstep(.6,1.2,uv.y));col*=.55+.6*band;
      col=mix(col,vec3(dot(col,vec3(.33))),sc);col*=1.-sc*.35;
      gl_FragColor=vec4(col,1.);}`;
    const sh=(t,s)=>{const o=gl.createShader(t);gl.shaderSource(o,s);gl.compileShader(o);return o};
    const pr=gl.createProgram();gl.attachShader(pr,sh(gl.VERTEX_SHADER,vs));gl.attachShader(pr,sh(gl.FRAGMENT_SHADER,fs));gl.linkProgram(pr);gl.useProgram(pr);
    const b=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,b);gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,3,-1,-1,3]),gl.STATIC_DRAW);
    const loc=gl.getAttribLocation(pr,'p');gl.enableVertexAttribArray(loc);gl.vertexAttribPointer(loc,2,gl.FLOAT,false,0,0);
    const ur=gl.getUniformLocation(pr,'r'),ut=gl.getUniformLocation(pr,'t'),um=gl.getUniformLocation(pr,'m'),us=gl.getUniformLocation(pr,'sc');
    const DPR=Math.min(devicePixelRatio,1.25)*.6;
    const rs=()=>{cv.width=cv.clientWidth*DPR;cv.height=cv.clientHeight*DPR;gl.viewport(0,0,cv.width,cv.height)};rs();addEventListener('resize',rs);
    $('#hero').addEventListener('pointermove',e=>{const r=cv.getBoundingClientRect();mouse=[(e.clientX-r.left)/r.width,1-(e.clientY-r.top)/r.height]});
    let t0=performance.now(),last=0,fieldT=0,sc=0;
    // honour the project: stop rendering when hero not visible
    new IntersectionObserver(es=>{fieldOn=es[0].isIntersecting;$('#hudDec').textContent=fieldOn?'ACTIVE':'TORN DOWN'}).observe($('#hero'));
    (function draw(now){requestAnimationFrame(draw);if(!fieldOn){last=now;return}
      const dt=Math.min(now-last,50);last=now;fieldT+=dt/1000;
      smouse[0]+=(mouse[0]-smouse[0])*.05;smouse[1]+=(mouse[1]-smouse[1])*.05;
      gl.uniform2f(ur,cv.width,cv.height);gl.uniform1f(ut,fieldT*(RM?0:1)+20);gl.uniform2f(um,smouse[0],smouse[1]);gl.uniform1f(us,sc);
      gl.drawArrays(gl.TRIANGLES,0,3);
      const s=fieldT;const mm=Math.floor(s/60),ss=(s%60).toFixed(3).padStart(6,'0');$('#hudT').textContent=`${String(mm).padStart(2,'0')}:${ss}`;
    })(0);
  }
  
  /* ---------- loader + intro ---------- */
  const title=$('#htitle');title.innerHTML=[...'LiveWall'].map((c,i)=>`<span class="ch" style="background-position:${i/7*100}% 0">${c}</span>`).join('');
  const lo={v:0};
  const intro=gsap.timeline();
  intro.to(lo,{v:100,duration:RM?.1:1.6,ease:'power2.inOut',onUpdate(){$('#ln').textContent=String(Math.round(lo.v)).padStart(3,'0');$('#loader .bar').style.width=lo.v+'%'}})
   .to('#loader',{yPercent:-100,duration:.9,ease:'expo.inOut'})
   .from('#htitle .ch',{yPercent:110,rotate:8,opacity:0,stagger:.05,duration:1.1,ease:'expo.out'},'-=.45')
   .from('.hero-in',{y:30,opacity:0,stagger:.1,duration:.9,ease:'power3.out'},'-=.8')
   .set('#loader',{display:'none'});
  
  /* hero title parallax/explode on scroll */
  gsap.to('#htitle .ch',{yPercent:i=>(i%2?-1:1)*40,rotate:i=>(i-4)*4,opacity:.0,ease:'none',stagger:{each:.02},scrollTrigger:{trigger:'#hero',start:'top top',end:'bottom top',scrub:true}});
  gsap.to('#hero .content',{y:-120,ease:'none',scrollTrigger:{trigger:'#hero',start:'top top',end:'bottom top',scrub:true}});
  
  /* progress */
  gsap.to('#progress',{width:'100%',ease:'none',scrollTrigger:{start:0,end:'max',scrub:.2}});
  
  /* marquee duplication + velocity skew */
  const mqt=$('#mq');mqt.innerHTML+=mqt.innerHTML;
  let skew=gsap.quickTo(mqt,'skewX',{duration:.5});
  ScrollTrigger.create({onUpdate:s=>skew(gsap.utils.clamp(-12,12,s.getVelocity()/-300))});
  
  /* split headings */
  $$('.split').forEach(h=>{
    const walk=n=>{[...n.childNodes].forEach(c=>{if(c.nodeType===3){const f=document.createDocumentFragment();c.textContent.split(/(\s+)/).forEach(w=>{if(!w)return;if(/^\s+$/.test(w)){f.append(w);return}const o=document.createElement('span');o.className='w';o.innerHTML=`<span>${w}</span>`;f.append(o)});c.replaceWith(f)}else walk(c)})};
    walk(h);
    gsap.from(h.querySelectorAll('.w>span'),{yPercent:110,rotate:4,duration:1,ease:'expo.out',stagger:.04,scrollTrigger:{trigger:h,start:'top 85%'}});
  });
  $$('.sec-h p, .sec-h .idx').forEach(p=>gsap.from(p,{y:24,opacity:0,duration:1,ease:'power3.out',scrollTrigger:{trigger:p,start:'top 90%'}}));
  
  /* manifesto word reveal */
  const mt=$('#mtext');mt.innerHTML=mt.textContent.split(' ').map(w=>`<span class="word">${w}</span>`).join(' ');
  gsap.to('#mtext .word',{opacity:1,stagger:.1,ease:'none',scrollTrigger:{trigger:'#mtext',start:'top 80%',end:'bottom 45%',scrub:true}});
  // meters
  const mk=(el,n)=>{for(let i=0;i<n;i++)el.append(document.createElement('i'))};mk($('#mBad'),48);mk($('#mGood'),48);
  const bad=[...$('#mBad').children],good=[...$('#mGood').children];let mtick=0;
  setInterval(()=>{mtick++;bad.forEach((b,i)=>b.style.height=(40+40*Math.abs(Math.sin(i*.6+mtick*.3))+Math.random()*20)+'%');good.forEach(b=>b.style.height='2%')},120);
  
  /* count up */
  $$('.count').forEach(el=>{const to=+el.dataset.to,dec=+(el.dataset.dec||0),suf=el.dataset.suf||'';const o={v:0};
    gsap.to(o,{v:to,duration:2,ease:'power3.out',scrollTrigger:{trigger:el,start:'top 85%'},onUpdate:()=>el.textContent=o.v.toFixed(dec)+suf})});
  
  /* ---------- DEMO: coverage gate ---------- */
  const desk=$('#desk'),gcv=$('#gridCv'),gctx=gcv.getContext('2d'),vid=$('#deskVid');
  const GW=64,GH=40;let playing=true,pendingStop=null,uncovered=1,cpuHist=[];
  const logEl=$('#log');const log=(h)=>{const d=document.createElement('div');const t=new Date();d.innerHTML=`<span>${t.toLocaleTimeString([], {hour12:false})}</span> ${h}`;logEl.prepend(d);while(logEl.children.length>12)logEl.lastChild.remove()};
  log('<b>start</b> decoder created · 1 frame in flight');
  function measure(){
    const R=desk.getBoundingClientRect();const wins=$$('.win').map(w=>{const r=w.getBoundingClientRect();return{l:(r.left-R.left)/R.width,t:(r.top-R.top)/R.height,r:(r.right-R.left)/R.width,b:(r.bottom-R.top)/R.height}});
    let free=0;const cov=new Uint8Array(GW*GH);
    for(let y=0;y<GH;y++)for(let x=0;x<GW;x++){const px=(x+.5)/GW,py=(y+.5)/GH;let c=false;for(const w of wins)if(px>=w.l&&px<=w.r&&py>=w.t&&py<=w.b){c=true;break}cov[y*GW+x]=c;if(!c)free++}
    uncovered=free/(GW*GH);
    // draw grid
    const W=gcv.width=gcv.clientWidth*devicePixelRatio,H=gcv.height=gcv.clientHeight*devicePixelRatio;gctx.clearRect(0,0,W,H);
    const cw=W/GW,ch=H/GH;
    for(let y=0;y<GH;y++)for(let x=0;x<GW;x++){if(!cov[y*GW+x]){gctx.fillStyle='rgba(94,242,196,.28)';gctx.fillRect(x*cw+1,y*ch+1,cw-2,ch-2)}else{gctx.strokeStyle='rgba(255,255,255,.06)';gctx.strokeRect(x*cw,y*ch,cw,ch)}}
    // gate with hysteresis
    if(playing&&uncovered<.08){if(!pendingStop){log(`uncovered <b>${(uncovered*100).toFixed(1)}%</b> &lt; 8% · teardown in 0.4 s`);pendingStop=setTimeout(()=>{setPlay(false);pendingStop=null},400)}}
    else if(pendingStop&&uncovered>=.08){clearTimeout(pendingStop);pendingStop=null;log('recovered within 0.4 s · teardown cancelled')}
    if(!playing&&uncovered>.15)setPlay(true);
    $('#gfill').style.width=(uncovered*100)+'%';$('#gval').textContent=(uncovered*100).toFixed(1)+'%';
  }
  function setPlay(p){if(p===playing)return;playing=p;desk.classList.toggle('stopped',!p);
    $('#state').classList.toggle('off',!p);$('#state span').textContent=p?'Playing':'Torn down';
    $('#badgeTxt').textContent=p?'DECODING · 24 fps':'LAST FRAME · 0 decode';
    if(p){vid.play().catch(()=>{});log(`uncovered <b>${(uncovered*100).toFixed(1)}%</b> &gt; 15% · <b>decoder recreated</b>, resume from saved ts`)}
    else{vid.pause();log('<b>decoder destroyed</b> · last frame stays composited')}}
  // cpu/mem readout + sparkline
  setInterval(()=>{const cpu=playing?2.6+Math.random()*.6:0,mem=playing?19:12;$('#rcpu').textContent=cpu.toFixed(1)+'%';$('#rmem').textContent=mem+' MB';
    cpuHist.push(cpu);if(cpuHist.length>60)cpuHist.shift();$('#sparkP').setAttribute('d',cpuHist.map((v,i)=>`${i?'L':'M'}${i*5},${44-v*12}`).join(''));
    $('#sparkP').setAttribute('stroke',playing?'#5ef2c4':'#ff4d8d')},250);
  // drag + resize
  $$('.win').forEach(w=>{
    let mode=null,sx,sy,sl,st,sw,sh;
    w.addEventListener('pointerdown',e=>{const R=desk.getBoundingClientRect();mode=e.target.classList.contains('rs')?'r':'m';sx=e.clientX;sy=e.clientY;sl=w.offsetLeft;st=w.offsetTop;sw=w.offsetWidth;sh=w.offsetHeight;w.setPointerCapture(e.pointerId);$$('.win').forEach(o=>o.style.zIndex=3);w.style.zIndex=4;e.preventDefault()});
    w.addEventListener('pointermove',e=>{if(!mode)return;const R=desk.getBoundingClientRect(),dx=e.clientX-sx,dy=e.clientY-sy;
      if(mode==='m'){w.style.left=((sl+dx)/R.width*100)+'%';w.style.top=((st+dy)/R.height*100)+'%'}
      else{w.style.width=(Math.max(80,sw+dx)/R.width*100)+'%';w.style.height=(Math.max(60,sh+dy)/R.height*100)+'%'}measure()});
    w.addEventListener('pointerup',()=>mode=null);
  });
  const init=[['6%','14%','44%','52%'],['46%','30%','46%','58%']];
  $('#tGrid').onclick=e=>{desk.classList.toggle('showgrid');e.target.classList.toggle('on')};
  $('#tMax').onclick=()=>{gsap.to('#w2',{left:'0%',top:'0%',width:'100%',height:'100%',duration:.6,ease:'expo.inOut',onUpdate:measure});};
  $('#tReset').onclick=()=>{$$('.win').forEach((w,i)=>gsap.to(w,{left:init[i][0],top:init[i][1],width:init[i][2],height:init[i][3],duration:.6,ease:'expo.inOut',onUpdate:measure}))};
  addEventListener('resize',measure);
  // demo video only decodes while demo visible — practise what we preach
  new IntersectionObserver(es=>{if(es[0].isIntersecting){if(playing)vid.play().catch(()=>{})}else vid.pause()}).observe(desk);
  setTimeout(measure,100);
  // auto-hint: show the gate once on first view
  ScrollTrigger.create({trigger:desk,start:'top 60%',once:true,onEnter:()=>{
    if(RM)return;desk.classList.add('showgrid');$('#tGrid').classList.add('on');
    gsap.timeline().to('#w2',{left:'0%',top:'0%',width:'100%',height:'96%',duration:1.4,ease:'expo.inOut',onUpdate:measure},.6)
      .to('#w2',{left:init[1][0],top:init[1][1],width:init[1][2],height:init[1][3],duration:1.2,ease:'expo.inOut',onUpdate:measure},'+=1.6')
      .call(()=>{desk.classList.remove('showgrid');$('#tGrid').classList.remove('on')},null,'+=.8');
  }});
  setInterval(()=>{const d=new Date();$('#clock').textContent=d.toLocaleTimeString([], {hour:'numeric',minute:'2-digit'})},1000);
  
  /* ---------- Horizontal decisions ---------- */
  const track=$('#htrack');
  const mmq=gsap.matchMedia();
  mmq.add('(min-width:821px)',()=>{
    const dist=()=>track.scrollWidth-innerWidth;
    const tw=gsap.to(track,{x:()=>-dist(),ease:'none',scrollTrigger:{trigger:'#decisions',pin:true,start:'top top',end:()=>'+='+dist(),scrub:1,invalidateOnRefresh:true}});
    $$('.dcard').forEach(c=>gsap.from(c,{rotateY:-18,scale:.88,opacity:.3,transformOrigin:'left center',ease:'none',scrollTrigger:{trigger:c,containerAnimation:tw,start:'left 100%',end:'left 50%',scrub:true}}));
  });
  
  /* decision visualisations (canvas, only run while visible) */
  const vizzes=[];
  $$('canvas[data-viz]').forEach(c=>{const ctx=c.getContext('2d');const v={c,ctx,type:c.dataset.viz,on:false,t:0};vizzes.push(v);
    new IntersectionObserver(es=>v.on=es[0].isIntersecting).observe(c)});
  const A1='#5ef2c4',A2='#7c6bff',A3='#ff8a4c',A4='#ff4d8d',DIM='rgba(255,255,255,.18)';
  function rr(ctx,x,y,w,h,r){ctx.beginPath();ctx.roundRect?ctx.roundRect(x,y,w,h,r):ctx.rect(x,y,w,h)}
  const VZ={
   teardown(ctx,W,H,t){const ph=(t%6)/6,vis=ph<.5;ctx.font=`${12*devicePixelRatio}px JetBrains Mono`;
     // desktop
     const dw=W*.5,dh=H*.6,dx=W*.06,dy=H*.2;const g=ctx.createLinearGradient(dx,dy,dx+dw,dy+dh);g.addColorStop(0,A2);g.addColorStop(1,A1);ctx.fillStyle=g;ctx.globalAlpha=.85;rr(ctx,dx,dy,dw,dh,10);ctx.fill();ctx.globalAlpha=1;
     if(vis){ctx.save();ctx.beginPath();rr(ctx,dx,dy,dw,dh,10);ctx.clip();for(let i=0;i<6;i++){ctx.fillStyle='rgba(255,255,255,.12)';ctx.fillRect(dx+((t*80*devicePixelRatio+i*60*devicePixelRatio)%dw),dy,2*devicePixelRatio,dh)}ctx.restore()}
     // cover window slides in
     const k=vis?0:Math.min(1,(ph-.5)*6);const cw=dw*k;ctx.fillStyle='#1a1d29';rr(ctx,dx,dy,cw,dh,10);ctx.fill();
     // decoder chip
     const cx=W*.64,cy=H*.38,cW=W*.3,cH=H*.24;ctx.strokeStyle=vis?A1:A4;ctx.lineWidth=2*devicePixelRatio;ctx.setLineDash(vis?[]:[6,6]);rr(ctx,cx,cy,cW,cH,10);ctx.stroke();ctx.setLineDash([]);
     ctx.fillStyle=vis?A1:A4;ctx.fillText(vis?'DECODER':'DESTROYED',cx+12*devicePixelRatio,cy+cH/2+4*devicePixelRatio);
     ctx.fillStyle='rgba(255,255,255,.5)';ctx.fillText(vis?'2.9% · 19 MB':'0.0% · 12 MB',cx,cy+cH+22*devicePixelRatio);
     if(!vis&&k>=1){ctx.fillStyle='rgba(255,255,255,.5)';ctx.fillText('last frame kept by compositor',dx,dy+dh+22*devicePixelRatio)}},
   coverage(ctx,W,H,t){const gw=32,gh=20,cw=W/gw,ch=H/gh;const wx=(.5+.45*Math.sin(t*.7))*W,wy=(.5+.4*Math.cos(t*.5))*H;const ww=W*.95,wh=H*.95;
     let free=0;for(let y=0;y<gh;y++)for(let x=0;x<gw;x++){const px=(x+.5)*cw,py=(y+.5)*ch;const c=Math.abs(px-wx)<ww/2&&Math.abs(py-wy)<wh/2;if(!c){free++;ctx.fillStyle='rgba(94,242,196,.45)';ctx.fillRect(x*cw+1,y*ch+1,cw-2,ch-2)}else{ctx.strokeStyle='rgba(255,255,255,.05)';ctx.strokeRect(x*cw,y*ch,cw,ch)}}
     ctx.strokeStyle='rgba(255,255,255,.6)';ctx.lineWidth=1.5*devicePixelRatio;ctx.strokeRect(wx-ww/2,wy-wh/2,ww,wh);
     const f=free/(gw*gh);v_cov.state=v_cov.state?f>.08||(f>.15):f>.15;if(f<.08)v_cov.state=false;if(f>.15)v_cov.state=true;
     ctx.font=`600 ${22*devicePixelRatio}px Space Grotesk`;ctx.fillStyle='#fff';ctx.fillText((f*100).toFixed(1)+'% uncovered',14*devicePixelRatio,30*devicePixelRatio);
     ctx.font=`${11*devicePixelRatio}px JetBrains Mono`;ctx.fillStyle=v_cov.state?A1:A4;ctx.fillText(v_cov.state?'● decoding':'● torn down',14*devicePixelRatio,50*devicePixelRatio)},
   oneframe(ctx,W,H,t){const d=devicePixelRatio;ctx.font=`${11*d}px JetBrains Mono`;const y=H/2,x0=W*.1,x1=W*.9;
     ctx.strokeStyle=DIM;ctx.lineWidth=1*d;ctx.beginPath();ctx.moveTo(x0,y);ctx.lineTo(x1,y);ctx.stroke();
     [['decoder',x0],['tick',W*.5],['compositor',x1]].forEach(([l,x])=>{ctx.fillStyle='#0b0d14';ctx.strokeStyle='rgba(255,255,255,.3)';ctx.beginPath();ctx.arc(x,y,16*d,0,7);ctx.fill();ctx.stroke();ctx.fillStyle='rgba(255,255,255,.6)';ctx.textAlign='center';ctx.fillText(l,x,y+38*d)});
     const p=(t*.9)%1;const fx=x0+(x1-x0)*(p<.5?p*2*.5:.5+(p-.5)*2*.5);const pulse=Math.abs(Math.sin(t*3));
     ctx.fillStyle=A1;ctx.shadowColor=A1;ctx.shadowBlur=20*d;rr(ctx,fx-14*d,y-10*d,28*d,20*d,4*d);ctx.fill();ctx.shadowBlur=0;ctx.textAlign='left';
     ctx.fillStyle='rgba(255,255,255,.55)';ctx.fillText('frames buffered: 1',14*d,24*d);ctx.fillStyle=A1;ctx.globalAlpha=.4+.6*pulse;ctx.fillText('DisplayImmediately',14*d,42*d);ctx.globalAlpha=1},
   format(ctx,W,H,t){const d=devicePixelRatio;ctx.font=`${11*d}px JetBrains Mono`;
     const bars=[['named format',65,A4],['native 4:2:0',4,A1]];const mx=65;
     bars.forEach(([l,v,c],i)=>{const y=H*.25+i*H*.32;const k=Math.min(1,((t*.6)%3));ctx.fillStyle='rgba(255,255,255,.55)';ctx.fillText(l,16*d,y-8*d);ctx.fillStyle='rgba(255,255,255,.05)';rr(ctx,16*d,y,W-32*d,26*d,6*d);ctx.fill();
       ctx.fillStyle=c;rr(ctx,16*d,y,(W-32*d)*v/mx*k,26*d,6*d);ctx.fill();ctx.fillStyle='#fff';ctx.font=`700 ${16*d}px Space Grotesk`;ctx.fillText(Math.round(v*k)+' samples',22*d,y+19*d);ctx.font=`${11*d}px JetBrains Mono`});
     ctx.fillStyle='rgba(255,255,255,.4)';ctx.fillText('IOConnectCallMethod → mach_msg2_trap per frame ✕',16*d,H-16*d)},
   import(ctx,W,H,t){const d=devicePixelRatio;ctx.font=`${11*d}px JetBrains Mono`;const items=['VP9','60fps','AUDIO','B-FRM','8K','AV1','ProRes','moov@EOF'];
     const gx=W*.5;ctx.strokeStyle=A2;ctx.lineWidth=2*d;ctx.beginPath();ctx.moveTo(gx,H*.15);ctx.lineTo(gx,H*.85);ctx.stroke();ctx.fillStyle=A2;ctx.textAlign='center';ctx.fillText('IMPORT',gx,H*.1);
     items.forEach((it,i)=>{const p=((t*.25+i/items.length)%1);const x=W*.05+p*W*.9,y=H*.22+(i%4)*H*.17;const after=x>gx;
       ctx.globalAlpha=Math.min(1,Math.min(p,1-p)*8);ctx.fillStyle=after?'rgba(94,242,196,.15)':'rgba(255,77,141,.15)';ctx.strokeStyle=after?A1:A4;rr(ctx,x-34*d,y-11*d,68*d,22*d,11*d);ctx.fill();ctx.stroke();
       ctx.fillStyle=after?A1:A4;ctx.fillText(after?'HEVC ✓':it,x,y+4*d)});ctx.globalAlpha=1;ctx.textAlign='left'}
  };
  const v_cov={state:true};
  let vlast=performance.now();
  (function vloop(now){requestAnimationFrame(vloop);const dt=(now-vlast)/1000;vlast=now;
    for(const v of vizzes){if(!v.on)continue;const c=v.c,W=c.clientWidth*devicePixelRatio,H=c.clientHeight*devicePixelRatio;if(c.width!==W||c.height!==H){c.width=W;c.height=H}
      v.t+=RM?0:dt;v.ctx.clearRect(0,0,W,H);VZ[v.type](v.ctx,W,H,v.t)}})(vlast);
  
  /* ---------- Chart ---------- */
  const rows=[['1280×720','1.4 Mbps · 8-bit',1.0,15,3.00],['1920×1080','3.1 Mbps · 8-bit',2.3,16,3.14],['1920×1080','6.8 Mbps · 8-bit',2.3,18,2.74],['3492×1964','10 Mbps · 8-bit',7.4,18,3.44],['1920×1080','5.6 Mbps · 10-bit',2.3,18,2.92],['1920×1080','6.8 Mbps · 60 fps',2.3,19,7.78]];
  const chart=$('#chart');
  rows.forEach(([a,b,px,mem,cpu],i)=>{const hot=cpu>5;const r=document.createElement('div');r.className='crow'+(hot?' hot':'');
    r.innerHTML=`<div class="lab">${a}<small>${b} · ${mem} MB</small></div><div class="bar"><u style="--w:${px/7.4*100}%"></u><i style="--w:${cpu/8*100}%"></i></div><div class="val" data-v="${cpu}">0.00%</div>`;chart.append(r)});
  ScrollTrigger.create({trigger:chart,start:'top 75%',once:true,onEnter:()=>{
    $$('.crow').forEach((r,i)=>{const b=r.querySelector('i'),u=r.querySelector('u'),v=r.querySelector('.val');
      gsap.to(u,{width:u.style.getPropertyValue('--w'),duration:1.2,delay:i*.1,ease:'expo.out'});
      gsap.to(b,{width:b.style.getPropertyValue('--w'),duration:1.6,delay:.3+i*.12,ease:'expo.out'});
      const o={x:0};gsap.to(o,{x:+v.dataset.v,duration:1.6,delay:.3+i*.12,ease:'expo.out',onUpdate:()=>v.textContent=o.x.toFixed(2)+'%'})})}});
  
  /* ---------- Pipeline lanes ---------- */
  function lane(cv,qmax,memEl,perMB){const ctx=cv.getContext('2d');let on=false,frames=[],acc=0,t=0,q=0;new IntersectionObserver(e=>on=e[0].isIntersecting).observe(cv);
    let last=performance.now();
    (function L(now){requestAnimationFrame(L);const dt=Math.min(.05,(now-last)/1000);last=now;if(!on)return;const d=devicePixelRatio,W=cv.width=cv.clientWidth*d,H=cv.height=cv.clientHeight*d;t+=dt;
      ctx.font=`${11*d}px JetBrains Mono`;
      const dec=W*.08,buf0=W*.22,buf1=W*.78,comp=W*.92,y=H/2;
      // decode fills buffer until qmax
      acc+=dt*(qmax>1?40:24);while(acc>1){acc--;if(frames.length<qmax)frames.push({p:0})}
      // consume at 24fps
      lane._c=(lane._c||0);
      // draw stations
      [['decode',dec],['present',comp]].forEach(([l,x])=>{ctx.fillStyle='rgba(255,255,255,.06)';ctx.strokeStyle='rgba(255,255,255,.25)';rr(ctx,x-30*d,y-24*d,60*d,48*d,10*d);ctx.fill();ctx.stroke();ctx.fillStyle='rgba(255,255,255,.6)';ctx.textAlign='center';ctx.fillText(l,x,y+44*d)});
      ctx.strokeStyle='rgba(255,255,255,.08)';ctx.strokeRect(buf0,y-28*d,buf1-buf0,56*d);
      // frames
      const n=frames.length,sp=(buf1-buf0)/32;
      frames.forEach((f,i)=>{const x=buf1-sp*(i+.5);ctx.fillStyle=qmax>1?`rgba(255,77,141,${.35+.5*(1-i/32)})`:'#5ef2c4';if(qmax===1){ctx.shadowColor='#5ef2c4';ctx.shadowBlur=18*d}rr(ctx,x-sp*.4,y-20*d,sp*.8,40*d,3*d);ctx.fill();ctx.shadowBlur=0});
      // consume one every 1/24
      q+=dt*24;while(q>1){q--;if(frames.length)frames.shift()}
      ctx.textAlign='left';ctx.fillStyle='rgba(255,255,255,.4)';ctx.fillText(`queue ${String(frames.length).padStart(2,' ')}`,buf0,y-38*d);
      memEl.textContent=Math.round(perMB(frames.length));
    })(last)}
  lane($('#lane1'),30,$('#pq1'),n=>12+n*.53);lane($('#lane2'),1,$('#pq2'),n=>19);
  
  /* ---------- Gates ---------- */
  const gates=[['◐','Desktop fully covered','teardown after 0.4 s'],['▦','Under 8% uncovered','resumes above 15%'],['⏻','Locked, asleep, screensaver','teardown'],['♨','Thermal serious / critical','teardown'],['🍃','Low Power Mode','teardown'],['▁','Battery below 20%','teardown'],['⚡','On battery at all','optional, off by default'],['☾','No input for 15 minutes','teardown']];
  const gEl=$('#gatesEl');gates.forEach(([i,h,s],k)=>{const d=document.createElement('div');d.className='gate';d.innerHTML=`<div class="st">armed</div><div><div class="ico">${i}</div><h6>${h}</h6><small>${s}</small></div>`;
    d.onclick=()=>{d.classList.toggle('tripped');d.querySelector('.st').textContent=d.classList.contains('tripped')?'tripped':'armed';gUpd()};
    d.onpointermove=e=>{const r=d.getBoundingClientRect();d.style.setProperty('--mx',(e.clientX-r.left)+'px');d.style.setProperty('--my',(e.clientY-r.top)+'px')};gEl.append(d)});
  function gUpd(){const n=$$('.gate.tripped').length;$('#gpill').textContent=n?`${n} signal${n>1?'s':''} tripped`:'all clear';const s=$('#gstate');s.textContent=n?'destroyed · last frame on screen':'running';s.style.color=n?'var(--a4)':'var(--ink)';gsap.fromTo(s,{y:10,opacity:0},{y:0,opacity:1,duration:.4})}
  gsap.from('.gate',{y:60,opacity:0,stagger:.06,duration:.9,ease:'expo.out',scrollTrigger:{trigger:'#gatesEl',start:'top 80%'}});
  
  /* import rows */
  gsap.from('#imp .row:not(.head)>div',{x:i=>i%2?40:-40,opacity:0,stagger:.05,duration:.8,ease:'power3.out',scrollTrigger:{trigger:'#imp',start:'top 80%'}});
  ScrollTrigger.create({trigger:'#imp',start:'top 60%',once:true,onEnter:()=>$$('#imp s').forEach((s,i)=>gsap.fromTo(s,{textDecorationColor:'rgba(255,77,141,0)'},{textDecorationColor:'rgba(255,77,141,1)',delay:.6+i*.12,duration:.4}))});
  gsap.from('.preset',{y:50,opacity:0,stagger:.1,duration:1,ease:'expo.out',scrollTrigger:{trigger:'.presets',start:'top 85%'}});
  
  /* ---------- Platforms tilt ---------- */
  $$('.tilt').forEach(c=>{c.addEventListener('pointermove',e=>{const r=c.getBoundingClientRect(),x=(e.clientX-r.left)/r.width,y=(e.clientY-r.top)/r.height;c.style.transform=`rotateY(${(x-.5)*14}deg) rotateX(${(.5-y)*14}deg)`;c.style.setProperty('--gx',x*100+'%');c.style.setProperty('--gy',y*100+'%')});c.addEventListener('pointerleave',()=>c.style.transform='')});
  gsap.from('.plat',{y:100,opacity:0,rotateX:-20,stagger:.1,duration:1.2,ease:'expo.out',scrollTrigger:{trigger:'.plats',start:'top 80%'}});
  
  const diffs=[['Behind icons','<code>kCGDesktopWindowLevel</code>. Documented, and it takes one call.','Undocumented <code>WorkerW</code> parenting via message <code>0x052C</code>. Checked after the fact, with a fallback if it fails.'],
   ['Occlusion','<code>NSWindow.occlusionState</code> fires on its own. The window list only adds detail.','A WorkerW child gets no occlusion signal at all. The window-list poll every 4 s is the only gate.'],
   ['Procedural','<code>CAGradientLayer</code>. The render server interpolates it, so it costs the app nothing.','A 15 fps pixel shader, because Windows has no compositor-side gradient.'],
   ['Thermal','<code>ProcessInfo.thermalState</code>.','Windows has no guaranteed thermal API, so Battery Saver stands in for it.'],
   ['Codec','HEVC Main10 is guaranteed on every Mac.','Probed at runtime, falling back HEVC Main10 → HEVC Main → H.264.'],
   ['Login item','<code>SMAppService</code>. It\u2019s tied to bundle identity, so a rebuild breaks it silently.','<code>HKCU\\…\\Run</code> stores a path, and a stale one is repaired on the next launch.']];
  const dt=$('#dtabs');diffs.forEach((d,i)=>{const b=document.createElement('button');b.textContent=d[0];b.onclick=()=>showDiff(i);dt.append(b)});
  function showDiff(i){$$('#dtabs button').forEach((b,k)=>b.classList.toggle('on',k===i));const [_,m,w]=diffs[i];
    gsap.to(['#dmac','#dwin'],{opacity:0,y:-10,duration:.18,onComplete(){$('#dmac').innerHTML=m;$('#dwin').innerHTML=w;gsap.fromTo(['#dmac','#dwin'],{opacity:0,y:14},{opacity:1,y:0,duration:.5,stagger:.08,ease:'power3.out'})}})}
  showDiff(0);let dIdx=0,dTimer=setInterval(()=>{dIdx=(dIdx+1)%diffs.length;showDiff(dIdx)},4200);$('#dtabs').addEventListener('click',()=>clearInterval(dTimer));
  
  /* ---------- Samples: lazy play only in view ---------- */
  $$('video[data-lazy]').forEach(v=>new IntersectionObserver(e=>{e[0].isIntersecting?v.play().catch(()=>{}):v.pause()},{threshold:.25}).observe(v));
  gsap.from('.sample',{clipPath:'inset(30% 30% 30% 30% round 22px)',duration:1.4,ease:'expo.inOut',stagger:.15,scrollTrigger:{trigger:'.samples',start:'top 80%'}});
  
  /* ---------- Terminal ---------- */
  const T={
   macOS:[['c','# build, install to /Applications, launch'],['p','./tools/install.sh'],['o','==> built LiveWall.app (504 KB binary)'],['o','==> installed · status bar item ready'],['p','swift test'],['o','✔ 31 tests, no device or display needed']],
   Windows:[['c','# per-user install, no admin'],['p','./tools/install.ps1'],['o','installed to %LOCALAPPDATA%\\Programs\\LiveWall'],['p','LiveWall.exe --probe'],['o','HEVC Main10 decode: yes · encode: yes'],['p','ctest --preset default'],['o','✔ 45 tests passed']],
   Linux:[['p','livewall probe'],['o','backend     x11 (occlusion aware)'],['o','libavcodec  found · libva found'],['p','livewall add ~/Videos/aurora.mp4'],['o','imported · playing'],['p','livewall status']],
   Android:[['c','# 102 KB release APK'],['p','adb install LiveWall-android.apk'],['o','Success'],['c','# Settings → Wallpaper → LiveWall'],['o','hidden: 0.00% CPU, MediaCodec released']]};
  const tt=$('#ttabs'),tout=$('#tout');let ttimer;
  Object.keys(T).forEach((k,i)=>{const b=document.createElement('button');b.textContent=k;b.onclick=()=>type(k);tt.append(b)});
  function type(k){$$('#ttabs button').forEach(b=>b.classList.toggle('on',b.textContent===k));clearTimeout(ttimer);tout.innerHTML='';
    const lines=T[k];let li=0,ci=0,html='';
    const step=()=>{if(li>=lines.length){tout.innerHTML=html+'<span class="p">$ </span><span class="caret"></span>';return}
      const [ty,tx]=lines[li];
      if(ty==='o'){html+=tx.replace(/</g,'&lt;')+'\n';li++;tout.innerHTML=html+'<span class="caret"></span>';ttimer=setTimeout(step,160);return}
      const pre=ty==='p'?'<span class="p">$ </span>':'<span class="c">';const post=ty==='c'?'</span>':'';
      ci++;tout.innerHTML=html+pre+tx.slice(0,ci)+post+'<span class="caret"></span>';
      if(ci>=tx.length){html+=pre+tx+post+'\n';li++;ci=0;ttimer=setTimeout(step,380)}else ttimer=setTimeout(step,22+Math.random()*40)};
    step()}
  ScrollTrigger.create({trigger:'.term',start:'top 80%',once:true,onEnter:()=>type('macOS')});
  
  /* footer */
  gsap.from('footer .cta',{yPercent:40,opacity:0,duration:1.4,ease:'expo.out',scrollTrigger:{trigger:'footer',start:'top 80%'}});
  
  /* smooth anchor */
  $$('a[href^="#"]').forEach(a=>a.addEventListener('click',e=>{const t=$(a.getAttribute('href'));if(!t)return;e.preventDefault();t.scrollIntoView({behavior:'smooth'})}));
  if(document.readyState==='complete')ScrollTrigger.refresh();else addEventListener('load',()=>ScrollTrigger.refresh());}
