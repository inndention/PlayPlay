/* Star News Motion Engine. No external dependencies. */
(() => {
  'use strict';
  const params = new URLSearchParams(location.search), $ = id => document.getElementById(id);
  const defaults = { scene:'id', speed:1, bug:false, tagline:'MORE LOCAL NEWS.', coverage:'MARTINSVILLE · HENRY COUNTY · DANVILLE · REIDSVILLE', stationLine:'getstarnews.com', kicker:'COMMUNITY SPOTLIGHT', headline:'Your community. Your stories.', subtitle:'More local coverage from Star News', location:'MARTINSVILLE, VA', breakingHeadline:'Coverage that keeps you connected', breakingSubtitle:'Updates from across our community', upnextTitle:'THE LOCAL\nPERSPECTIVE.', upnextSubtitle:'The stories that matter. The people you know.', upnextTime:'NEXT ON STAR NEWS', upnextWebsite:'getstarnews.com', ticker:'STAR NEWS  •  More Local News.     ✦     Your community. Your connection.     ✦     Watch and connect at getstarnews.com', weatherLabel:'SAMPLE DATA', weatherRows:'Martinsville | 68 | Partly cloudy | 72 | 51\nDanville | 71 | Sunny | 75 | 53\nReidsville | 67 | Cloudy | 70 | 50\nEden | 69 | Clear | 73 | 52', weatherInterval:6 };
  Object.assign(defaults,{race:'SAMPLE RACE',candidateName:'SAMPLE CANDIDATE',candidateParty:'SAMPLE PARTY',candidateVotes:'—',candidatePercent:'—',resultsRows:'SAMPLE CANDIDATE A | PARTY | — | —\nSAMPLE CANDIDATE B | PARTY | — | —',resultsSource:'SAMPLE DATA · NOT ELECTION RESULTS'});
  const validScenes=['id','lower','breaking','weather','upnext','ticker','candidate','results'];
  let config={...defaults}, on=true, weatherIndex=0, weatherElapsed=0, weatherSwap=null, weatherRows=[], channel=null, lastSeq=-1, replayTimer=null;
  try { const data=JSON.parse(params.get('data') || '{}'); config={...config,...data}; } catch {}
  if(validScenes.includes(params.get('scene')))config.scene=params.get('scene');
  const stage=$('stage'), graphic=$('graphic');
  const safeNumber=(value,fallback,min,max)=>Number.isFinite(Number(value)) ? Math.max(min,Math.min(max,Number(value))) : fallback;
  function resize(){stage.style.transform=`translate(-50%,-50%) scale(${Math.min(innerWidth/1920,innerHeight/1080)})`;}
  addEventListener('resize',resize);resize();
  function fit(id,max,min){const el=$(id);el.style.fontSize=max+'px';let size=max;while(el.scrollWidth>el.clientWidth+1 && size>min){el.style.fontSize=(--size)+'px';}}
  function icon(condition){
    const c=condition.toLowerCase();
    const sun='<g class="sun-rays" stroke="#f5c86b" stroke-width="2.6" stroke-linecap="round"><path d="M32 2v7M32 55v7M2 32h7M55 32h7M11 11l5 5M48 48l5 5M11 53l5-5M48 16l5-5"/></g><circle cx="32" cy="32" r="13" fill="#f5ce79"/>';
    const cloud='<g class="cloud"><path d="M14 48c-15 0-15-22 0-23 4-20 33-18 36 0 20-1 20 23 3 23Z" fill="#d6e7f3"/></g>';
    let shape=sun;
    if(/rain|shower|storm/.test(c))shape=cloud+'<path d="M22 53l-3 7m16-7-3 7m16-7-3 7" stroke="#73b9ef" stroke-width="3"/>';
    else if(/snow/.test(c))shape=cloud+'<path d="M23 54v8m-4-4h8m16-4v8m-4-4h8" stroke="#fff" stroke-width="2"/>';
    else if(/partly|mostly/.test(c))shape='<g transform="translate(-4 -6) scale(.85)">'+sun+'</g>'+cloud;
    else if(/cloud|overcast|fog/.test(c))shape=cloud;
    else if(/night/.test(c))shape='<path d="M42 6A26 26 0 1 0 58 46 24 24 0 0 1 42 6" fill="#d4e5f8"/>';
    return '<svg viewBox="0 0 64 68" aria-hidden="true">'+shape+'</svg>';
  }
  function parseWeather(){
    weatherRows=String(config.weatherRows).split('\n').map(row=>row.split('|').map(x=>x.trim())).filter(r=>r.length>=3 && r[0]).slice(0,12);
    if(!weatherRows.length)weatherRows=[['LOCAL','—','No data','—','—']];
    weatherIndex %= weatherRows.length;weatherElapsed=0;clearTimeout(weatherSwap);$('weatherSlide').classList.remove('flipping');renderWeather();
  }
  function renderWeather(){const row=weatherRows[weatherIndex];$('weatherCity').textContent=row[0].toUpperCase();$('weatherTemp').textContent=row[1].replace(/°/g,'')+'°';$('weatherCondition').textContent=row[2].toUpperCase();$('weatherRange').textContent=`H ${(row[3]||'—').replace(/°/g,'')}° / L ${(row[4]||'—').replace(/°/g,'')}°`;$('weatherIcon').innerHTML=icon(row[2]);fit('weatherCity',19,11);}
  function apply(next){
    const oldRows=config.weatherRows;config={...config,...next};if(!validScenes.includes(config.scene))config.scene='id';config.speed=safeNumber(config.speed,1,.5,1.5);config.weatherInterval=safeNumber(config.weatherInterval,6,3,30);config.bug=!!config.bug;
    document.documentElement.style.setProperty('--speed',config.speed);
    for(const id of ['tagline','coverage','stationLine','kicker','headline','subtitle','location','breakingHeadline','breakingSubtitle','upnextTitle','upnextSubtitle','upnextTime','upnextWebsite','weatherLabel'])$(id).textContent=String(config[id]??defaults[id]);
    $('race').textContent=String(config.race||'');$('resultsSource').textContent=String(config.resultsSource||'SOURCE / STATUS NOT PROVIDED');
    const rows=config.scene==='candidate'?[[config.candidateName,config.candidateParty,config.candidateVotes,config.candidatePercent]]:String(config.resultsRows||'').split('\n').slice(0,6).map(x=>x.split('|'));
    $('electionRows').replaceChildren();for(const row of rows){const el=document.createElement('div');el.className='election-row';for(const [i,val] of row.slice(0,4).entries()){const cell=document.createElement('span');cell.className=['candidate-name','candidate-party','candidate-votes','candidate-percent'][i];cell.textContent=String(val||'').trim();el.append(cell);}$('electionRows').append(el);}
    const ticker=String(config.ticker||defaults.ticker)+'     ✦     ';for(const el of $('tickerTrack').children)el.textContent=ticker;
    $('tickerTrack').style.setProperty('--ticker-duration',Math.max(15,ticker.length*.16)/config.speed+'s');
    graphic.className=`graphic scene-${config.scene} ${on?'on':'off'}${config.bug?' show-bug':''}`;
    if(!weatherRows.length || oldRows!==config.weatherRows)parseWeather();
    requestAnimationFrame(()=>{fit('headline',49,22);fit('subtitle',27,17);fit('breakingHeadline',43,22);fit('breakingSubtitle',24,16);});
  }
  function cue(action){clearTimeout(replayTimer);if(action==='replay'){on=false;apply({});replayTimer=setTimeout(()=>{on=true;apply({});},850/config.speed);}else{on=action!=='out';apply({});}return on;}
  function receive(data){if(!data || data.type!=='star-news-control')return;if(data.seq!=null){if(data.seq<=lastSeq)return;lastSeq=data.seq;}if(data.config)apply(data.config);if(data.action)cue(data.action);else if(typeof data.on==='boolean' && data.on!==on)cue(data.on?'in':'out');}
  addEventListener('message',event=>{if(event.source===parent && (event.origin===location.origin || location.protocol==='file:'))receive(event.data);});
  if(params.has('embed')){try{channel=new BroadcastChannel('star-news-motion-v1');channel.onmessage=e=>receive(e.data);channel.postMessage({type:'star-news-ready'});}catch{}}
  const notifyReady=()=>{if(parent!==window)parent.postMessage({type:'star-news-ready'},location.origin==='null'?'*':location.origin);};
  window.StarNews={play:()=>cue('in'),stop:()=>cue('out'),replay:()=>cue('replay'),update:apply,select:scene=>{cue('out');setTimeout(()=>{apply({scene});cue('in');},850/config.speed);},getState:()=>({...config,on})};
  addEventListener('keydown',e=>{if(e.key==='Enter')cue('in');if(e.key==='Escape')cue('out');if(e.key.toLowerCase()==='r')cue('replay');});
  function clock(){const time=new Intl.DateTimeFormat('en-US',{timeZone:'America/New_York',hour:'numeric',minute:'2-digit'}).format(new Date());$('bugTime').textContent=time+' ET';$('tickerTime').textContent=time;}
  clock();setInterval(clock,1000);
  const canvas=$('stars'),ctx=canvas.getContext('2d');let seed=427;const random=()=>{seed=(seed*1664525+1013904223)>>>0;return seed/4294967296;};
  const stars=Array.from({length:460},()=>({x:random()*1920,y:random()*1080,r:random()<.94?random()*1.1+.25:random()*1.4+1.1,depth:random()*.8+.2,alpha:random()*.55+.15,phase:random()*Math.PI*2}));
  let last=performance.now(),elapsed=0;
  function frame(now){
    const realDt=(now-last)/1000,dt=Math.min(realDt,.06);last=now;elapsed+=dt*config.speed;
    if(on && (config.scene==='id'||config.scene==='upnext')){
      ctx.clearRect(0,0,1920,1080);
      for(const s of stars){s.x=(s.x+dt*s.depth*2.1*config.speed)%1920;s.y=(s.y+1080-dt*s.depth*.38*config.speed)%1080;const a=s.alpha*(.78+.22*Math.sin(elapsed*.45+s.phase));ctx.fillStyle=`rgba(201,224,255,${a})`;ctx.beginPath();ctx.arc(s.x,s.y,s.r,0,Math.PI*2);ctx.fill();if(s.r>1.8){ctx.strokeStyle=`rgba(170,210,255,${a*.3})`;ctx.lineWidth=.6;ctx.beginPath();ctx.moveTo(s.x-5,s.y);ctx.lineTo(s.x+5,s.y);ctx.moveTo(s.x,s.y-5);ctx.lineTo(s.x,s.y+5);ctx.stroke();}}
      const comet=elapsed%38;if(comet>31 && comet<32.7){const p=(comet-31)/1.7,x=1500-p*750,y=120+p*240;const g=ctx.createLinearGradient(x,y,x+160,y-52);g.addColorStop(0,'rgba(190,222,255,.55)');g.addColorStop(1,'rgba(190,222,255,0)');ctx.strokeStyle=g;ctx.lineWidth=1.4;ctx.beginPath();ctx.moveTo(x,y);ctx.lineTo(x+160,y-52);ctx.stroke();}
    }
    if(on && (config.scene==='weather'||config.bug)){
      weatherElapsed+=realDt;const interval=config.weatherInterval;$('weatherProgress').style.width=Math.min(100,weatherElapsed/interval*100)+'%';
      if(weatherElapsed>=interval){weatherElapsed=0;$('weatherSlide').classList.add('flipping');weatherSwap=setTimeout(()=>{weatherIndex=(weatherIndex+1)%weatherRows.length;renderWeather();$('weatherSlide').classList.remove('flipping');},320);}
    }
    requestAnimationFrame(frame);
  }
  apply(config);on=false;apply({});requestAnimationFrame(()=>{requestAnimationFrame(()=>{cue(params.has('live')||params.get('autoplay')==='0'?'out':'in');notifyReady();});});requestAnimationFrame(frame);
  if(params.has('live') && location.protocol!=='file:'){
    fetch('api/health',{cache:'no-store'}).then(r=>r.ok?r.json():null).then(data=>{if(data?.starNewsRelay){const poll=async()=>{try{const r=await fetch('api/state',{cache:'no-store'});if(r.ok){const state=await r.json();receive(state);}}catch{}setTimeout(poll,250);};poll();}}).catch(()=>{});
  }
})();
