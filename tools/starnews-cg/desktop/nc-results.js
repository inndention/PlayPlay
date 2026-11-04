'use strict';
const yauzl = require('yauzl');
function parseResults(text, county, contest) {
  if (!county.trim() || !contest.trim()) throw Error('Enter the exact county and contest name.');
  const lines=text.replace(/^\uFEFF/,'').split(/\r?\n/), header=lines.shift().split('\t').map(v=>v.trim().toLowerCase());
  const names=['county','contest name','choice','choice party','vote for','total votes'];
  const idx=names.map(n=>header.indexOf(n)); if(idx.some(i=>i<0)) throw Error('The state results file format is not recognized.');
  const counts=new Map(); let voteFor=null;
  for(const line of lines){if(!line)continue;const col=line.split('\t'); if((col[idx[0]]||'').trim().toUpperCase()!==county.trim().toUpperCase()||(col[idx[1]]||'').trim().toUpperCase()!==contest.trim().toUpperCase())continue;
    const name=(col[idx[2]]||'').trim(),party=(col[idx[3]]||'').trim(),raw=(col[idx[5]]||'').trim(),vf=Number(col[idx[4]]);
    if(!name||!/^\d+$/.test(raw)||!Number.isSafeInteger(Number(raw))||!Number.isInteger(vf)||vf<1) throw Error('The matching contest contains invalid data.');
    if(voteFor!==null&&voteFor!==vf)throw Error('Contest data has inconsistent vote-for counts.');voteFor=vf;
    const key=JSON.stringify([name,party]);const total=(counts.get(key)||0)+Number(raw);if(!Number.isSafeInteger(total))throw Error('Vote total exceeds safe range.');counts.set(key,total);
  }
  if(!counts.size)throw Error('No exact match. Check county, contest and election date against the NCSBE dashboard.');
  const rows=Array.from(counts,([key,votes])=>{const [name,party]=JSON.parse(key);return {name,party,votes};}).sort((a,b)=>b.votes-a.votes);
  if(rows.length>6)throw Error('More than six choices: the current graphic cannot display the full contest.');
  const total=rows.reduce((n,r)=>n+r.votes,0); return {rows,voteFor,total};
}
function unzip(buffer, expected){return new Promise((resolve,reject)=>yauzl.fromBuffer(buffer,{lazyEntries:true},(err,zip)=>{
  if(err)return reject(err);let done=false;const fail=e=>{if(!done){done=true;zip.close();reject(e);}};
  zip.on('error',fail);zip.on('end',()=>{if(!done)fail(Error('Expected results file missing from archive.'));});
  zip.on('entry',entry=>{if(entry.fileName!==expected)return zip.readEntry();if(entry.uncompressedSize>100*1024*1024)return fail(Error('Results file exceeds size limit.'));zip.openReadStream(entry,(e,stream)=>{if(e)return fail(e);let size=0,chunks=[];stream.on('error',fail);stream.on('data',chunk=>{size+=chunk.length;if(size>100*1024*1024){stream.destroy();fail(Error('Results file exceeds size limit.'));}else chunks.push(chunk);});stream.on('end',()=>{if(!done){done=true;zip.close();resolve(Buffer.concat(chunks).toString('utf8'));}});});});zip.readEntry();
}));}
async function download(date,county,contest){
  if(!/^\d{4}-\d{2}-\d{2}$/.test(date))throw Error('Enter an election date in YYYY-MM-DD format.');
  const [y,m,d]=date.split('-'), dt=new Date(`${date}T00:00:00Z`);if(!Number.isFinite(dt.getTime())||dt.toISOString().slice(0,10)!==date||date<'2018-07-01')throw Error('Enter a valid election date from July 2018 onward.');
  const stamp=y+m+d, url=`https://dl.ncsbe.gov/ENRS/${y}_${m}_${d}/results_pct_${stamp}.zip`;
  const response=await fetch(url,{redirect:'error',signal:AbortSignal.timeout(20000)});if(!response.ok)throw Error(`NCSBE file unavailable (HTTP ${response.status}).`);
  const declared=Number(response.headers.get('content-length')||0);if(declared>25*1024*1024)throw Error('NCSBE archive exceeds size limit.');
  let size=0, chunks=[];for await(const chunk of response.body){size+=chunk.length;if(size>25*1024*1024)throw Error('NCSBE archive exceeds size limit.');chunks.push(Buffer.from(chunk));}
  const text=await unzip(Buffer.concat(chunks),`results_pct_${stamp}.txt`);return {...parseResults(text,county,contest),url,fetchedAt:new Date().toISOString()};
}
module.exports={parseResults,download};
