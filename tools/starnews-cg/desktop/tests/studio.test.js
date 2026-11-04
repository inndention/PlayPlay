'use strict';
const {test} = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const {createRelay} = require('../relay');
const {parseResults} = require('../nc-results');
test('NC results sum precincts and administrative records only for exact county and contest',()=>{
  const header='County\tContest Name\tChoice\tChoice Party\tVote For\tTotal Votes';
  const data=[header,'ROCKINGHAM\tMAYOR\tA\tDEM\t1\t100','ROCKINGHAM\tMAYOR\tA\tDEM\t1\t20','ROCKINGHAM\tMAYOR\tB\tREP\t1\t60','OTHER\tMAYOR\tA\tDEM\t1\t500'].join('\n');
  const result=parseResults(data,'rockingham','mayor'); assert.equal(result.total,180);assert.deepEqual(result.rows.map(r=>r.votes),[120,60]);
  assert.throws(()=>parseResults(data,'rockingham','not mayor'),/No exact match/);
  assert.throws(()=>parseResults(data.replace('\t100','\t-100'),'rockingham','mayor'),/invalid data/);
});
test('output starts cleared and program snapshot is independent',async()=>{
  const relay=createRelay(path.join(__dirname,'../ui'));await new Promise(resolve=>relay.server.listen(0,'127.0.0.1',resolve));
  try {const base=`http://127.0.0.1:${relay.server.address().port}`;const initial=await (await fetch(base+'/api/state')).json();assert.equal(initial.on,false);
    const scene={scene:'results',race:'FIRST'};relay.setState(scene,'in');scene.race='EDITED';const state=await (await fetch(base+'/api/state')).json();assert.equal(state.config.race,'FIRST');
    assert.equal((await fetch(base+'/output.html')).status,200);assert.equal((await fetch(base+'/../main.js')).status,404);assert.equal((await fetch(base+'/api/state',{method:'POST'})).status,405);
    relay.setState(scene,'out');assert.equal(relay.getState().on,false);assert.equal(relay.getState().config.race,'FIRST');
  }finally{await new Promise(resolve=>relay.server.close(resolve));}
});
