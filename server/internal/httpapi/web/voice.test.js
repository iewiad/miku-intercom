import test from 'node:test';
import assert from 'node:assert/strict';
import {VoiceCommandClient} from './voice.js';
import {ControlClient} from './app.js';
const tick=async()=>{for(let i=0;i<12;i++)await Promise.resolve();};
const deferred=()=>{let resolve,reject;const promise=new Promise((a,b)=>{resolve=a;reject=b;});return{promise,resolve,reject};};
function fixture({respond,source,closeThrows=false,realControl=false}={}){
 const calls=[],timers=new Map();let timerID=0,peer;
 const track={enabled:false,stopped:false,stop(){this.stopped=true;}};
 const stream={getTracks:()=>[track],getAudioTracks:()=>[track]};
 const audio={syncs:0,suspends:0,sending:false,suspend(){this.suspends++;this.sending=false;},sync(){this.syncs++;},setSending(v){this.sending=v;},isReady:()=>true};
 const baseState={device:{id:'device_123',name:'甲'},server_epoch:'epoch_123',server_time:new Date().toISOString(),current_group_id:'group_a',current_version:1,groups:[{id:'group_a',name:'全员群'},{id:'group_b',name:'后勤组'}]};
 let currentState=structuredClone(baseState);
 const request=async(path,method='GET',body)=>{
  calls.push({path,method,body});
  const custom=respond?.(path,method,body);if(custom!==undefined)return await custom;
  if(path==='/voice')return{available:true,command_id:'',phase:'none'};
  if(path==='/media')return{peer_id:'old_group_peer'};
  if(path==='/voice/sessions')return{command_id:body.command_id};
  if(path.endsWith('/offer'))return{type:'answer',sdp:'v=0'};
  if(path.endsWith('/stop'))return{stopped:true,command_id:path.split('/')[3]};
  if(path==='/voice/resolve')return{group_id:'group_b',name:'后勤组'};
  if(path==='/state')return currentState;
  if(path==='/current-group'){
   currentState={...currentState,current_group_id:body.group_id,current_version:2};
   return{operation:{...body,status:'applied',current_group_id:body.group_id,current_version:2},state:currentState};
  }
  return{closed:true};
 };
 const control=realControl?new ControlClient({fetchImpl:async(url,opts)=>{try{const value=await request(url.replace('/api/v1',''),opts.method,opts.body?JSON.parse(opts.body):undefined);return{ok:true,status:200,json:async()=>value};}catch(error){if(error.status)return{ok:false,status:error.status,json:async()=>({error:{code:error.code}})};throw error;}},audio,onPending:p=>calls.push({pending:p}),schedule:(fn,ms)=>{const id=++timerID;timers.set(id,{fn,ms});return id;},cancel:id=>timers.delete(id)}):{
  state:currentState,connected:true,pendingSwitch:null,allowed:()=>true,mutationBusy:false,
  cancelFloor(){calls.push({cancelFloor:true});},async switchGroup(id){calls.push({switch:id});},async refresh(){calls.push({refresh:true});},
 };
 if(realControl){control.identity={device:baseState.device,token:'local-test-token'};control.state=currentState;control.connected=true;}
 const voice=new VoiceCommandClient({control,audio,request,source:source||(()=>Promise.resolve(stream)),random:()=>`command_123`,
  createPeer:()=>{
   const channel={sent:[],close(){this.closed=true;},send(text){this.sent.push(JSON.parse(text));}};
   peer={connectionState:'connected',iceGatheringState:'complete',channel,createDataChannel:()=>channel,
    addTransceiver:()=>{},createOffer:async()=>({type:'offer',sdp:'v=0'}),async setLocalDescription(value){this.localDescription=value;},async setRemoteDescription(value){this.answer=value;},
    close(){if(typeof closeThrows==='function'?closeThrows():closeThrows)throw new Error('close failed');this.connectionState='closed';},};
   return peer;
  },schedule:(fn,ms)=>{const id=++timerID;timers.set(id,{fn,ms});return id;},cancel:id=>timers.delete(id)});
 control.voice=voice;
 const message=e=>peer.channel.onmessage?.({data:JSON.stringify(e)});
 const ready=async()=>{await voice.initialize();await voice.start();message({type:'session.created'});message({type:'session.updated'});};
 return{voice,control,audio,calls,timers,track,get peer(){return peer;},message,ready};
}

test('explicit action closes group Peer before RTA; mic starts only after session updated; final executes once through durable switch',async()=>{
 const f=fixture({realControl:true});await f.voice.initialize();assert.equal(f.peer,undefined);await f.voice.start();
 assert.equal(f.track.enabled,false);assert.equal(f.audio.suspends>0,true);
 const deletion=f.calls.findIndex(c=>c.method==='DELETE');const creation=f.calls.findIndex(c=>c.path==='/voice/sessions');assert.ok(deletion>=0&&deletion<creation);
 f.message({type:'session.created'});const update=f.peer.channel.sent[0];assert.deepEqual(update.session.modalities,['text','audio']);assert.equal(update.session.turn_detection.create_response,false);assert.ok(!f.peer.channel.sent.some(e=>e.type==='response.create'));
 assert.equal(f.track.enabled,false);f.message({type:'session.updated'});assert.equal(f.track.enabled,true);
 f.message({type:'conversation.item.input_audio_transcription.delta',delta:'切到后勤组'});f.message({type:'response.text.done',text:'切到后勤组'});assert.equal(f.calls.some(c=>c.path==='/voice/resolve'),false);
 f.message({type:'conversation.item.input_audio_transcription.completed',transcript:'切到后勤组'});
 f.message({type:'conversation.item.input_audio_transcription.done',transcript:'切到全员群'});
 assert.equal(f.track.enabled,false);await tick();await tick();
 assert.equal(f.calls.filter(c=>c.path==='/voice/resolve').length,1);assert.equal(f.calls.filter(c=>c.path==='/current-group').length,1);
 assert.ok(f.calls.find(c=>c.pending?.operation_id));assert.equal(f.control.state.current_group_id,'group_b');
 assert.equal(f.track.stopped,true);assert.equal(f.peer.connectionState,'closed');assert.equal(f.voice.blocking,false);
 assert.equal(f.calls.filter(c=>c.path?.endsWith('/stop')).length,1);
});

test('cancel during resolution ignores late target and never submits switch',async()=>{
 const target=deferred();const f=fixture({respond:path=>path==='/voice/resolve'?target.promise:undefined});await f.ready();
 const late=f.peer.channel.onmessage;
 f.message({type:'conversation.item.input_audio_transcription.completed',transcript:'切到后勤组'});f.voice.interrupt();await tick();target.resolve({group_id:'group_b',name:'后勤组'});await tick();
 late({data:JSON.stringify({type:'conversation.item.input_audio_transcription.done',transcript:'切到后勤组'})});await tick();
 assert.equal(f.calls.some(c=>c.switch),false);assert.equal(f.voice.blocking,false);assert.equal(f.track.stopped,true);
});

test('cleanup failure keeps group audio and new commands blocked until same stop is acknowledged',async()=>{
 let stops=0;const f=fixture({respond:path=>{if(path.endsWith('/stop')){stops++;if(stops===1)return Promise.reject({code:'voice_cleanup_unconfirmed'});}}});await f.ready();
 const before=f.audio.syncs;f.voice.interrupt();await tick();assert.equal(f.voice.phase,'cleanup_pending');assert.equal(f.voice.blocking,true);assert.equal(f.audio.syncs,before);
 await f.voice.start();assert.equal(f.calls.filter(c=>c.path==='/voice/sessions').length,1);
 const retry=[...f.timers.values()].find(t=>t.ms===2000);assert.ok(retry);retry.fn();await tick();
 assert.equal(stops,2);assert.equal(f.voice.blocking,false);assert.ok(f.audio.syncs>before);
});

test('permission resolving after cancel is stopped and no RTA offer is posted',async()=>{
 const capture=deferred();const lateTrack={enabled:true,stopped:false,stop(){this.stopped=true;}};const f=fixture({source:()=>capture.promise});await f.voice.initialize();const start=f.voice.start();await tick();
 f.voice.interrupt();await tick();capture.resolve({getAudioTracks:()=>[lateTrack],getTracks:()=>[lateTrack]});await start;
 assert.equal(lateTrack.enabled,false);assert.equal(lateTrack.stopped,true);assert.equal(f.calls.some(c=>c.path?.endsWith('/offer')),false);
});

test('capture timeout stops microphone and restores only after session cleanup',async()=>{
 const f=fixture();await f.ready();assert.equal(f.track.enabled,true);const timeout=[...f.timers.values()].find(t=>t.ms===30000);timeout.fn();await tick();
 assert.equal(f.track.stopped,true);assert.equal(f.voice.blocking,false);assert.equal(f.calls.some(c=>c.switch),false);
});

test('reload discovers server session and cleans it before group recovery',async()=>{
 const stop=deferred();const f=fixture({respond:path=>path==='/voice'?{available:true,command_id:'previous_123',phase:'ready'}:path.endsWith('/stop')?stop.promise:undefined});
 const task=f.voice.initialize();await tick();assert.equal(f.voice.blocking,true);assert.equal(f.audio.syncs,0);
 stop.resolve({stopped:true,command_id:'previous_123'});await task;assert.equal(f.voice.blocking,false);assert.ok(f.audio.syncs>0);
});

test('local close failure never resumes group audio even when stop could succeed',async()=>{
 const f=fixture({closeThrows:true});await f.ready();const before=f.audio.syncs;f.voice.interrupt();await tick();assert.equal(f.voice.phase,'cleanup_pending');assert.equal(f.voice.blocking,true);assert.equal(f.audio.syncs,before);assert.equal(f.calls.some(c=>c.path?.endsWith('/stop')),true);
});

test('hidden after submitted switch stops RTA without undoing durable control operation',async()=>{
 const result=deferred();const f=fixture({realControl:true,respond:path=>path==='/current-group'?result.promise:undefined});await f.ready();
 f.message({type:'conversation.item.input_audio_transcription.completed',transcript:'切到后勤组'});await tick();assert.equal(f.control.pendingSwitch.group_id,'group_b');assert.equal(f.voice.canCancel,false);
 f.voice.interrupt('hidden');await tick();assert.equal(f.track.stopped,true);assert.equal(f.control.pendingSwitch.group_id,'group_b');assert.equal(f.control.canPress(),false);
 const op=f.control.pendingSwitch;result.resolve({operation:{...op,status:'applied',current_group_id:'group_b',current_version:2},state:{...f.control.state,current_group_id:'group_b',current_version:2}});await tick();await tick();assert.equal(f.control.pendingSwitch,null);
});

test('unavailable server has visible state and never asks for microphone',async()=>{
 const f=fixture({respond:path=>path==='/voice'?{available:false,command_id:''}:undefined,source:()=>{throw new Error('must not capture');}});await f.voice.initialize();await f.voice.start();assert.equal(f.voice.phase,'unavailable');assert.equal(f.voice.blocking,false);assert.equal(f.calls.some(c=>c.path==='/voice/sessions'),false);
});

for(const event of [{type:'error',error:{terminal:true,code:'provider_failed'}},{type:'rtid.error',error:{code:'invalid_session_update'}},{type:'conversation.item.input_audio_transcription.failed'}]){
 test(`provider failure ${event.type} stops capture immediately without a switch`,async()=>{const f=fixture();await f.ready();f.message(event);assert.equal(f.track.enabled,false);assert.equal(f.track.stopped,true);await tick();assert.equal(f.calls.some(c=>c.switch),false);assert.equal(f.voice.blocking,false);});
}

test('local close can fail repeatedly and later recover without losing scheduled retries',async()=>{
 let closes=0;const f=fixture({closeThrows:()=>++closes<3});await f.ready();f.voice.interrupt();await tick();assert.equal(f.voice.phase,'cleanup_pending');
 for(let i=0;i<2;i++){const entries=[...f.timers.entries()].filter(([,t])=>t.ms===2000);assert.ok(entries.length);const [id,retry]=entries.at(-1);f.timers.delete(id);retry.fn();await tick();}
 assert.equal(closes,3);assert.equal(f.voice.blocking,false);assert.equal(f.peer.connectionState,'closed');assert.equal(f.calls.filter(c=>c.path?.endsWith('/stop')).length,1);
});

test('storage failure in actual ControlClient leaves voice command unsubmitted and resumes only after RTA stop',async()=>{
 const f=fixture({realControl:true});f.control.onPending=()=>{throw new Error('QuotaExceededError');};await f.ready();
 f.message({type:'conversation.item.input_audio_transcription.completed',transcript:'切到后勤组'});await tick();await tick();
 assert.equal(f.calls.some(c=>c.path==='/current-group'),false);assert.equal(f.control.pendingSwitch,null);assert.equal(f.voice.blocking,false);
 assert.match(f.voice.message,/未提交/);assert.equal(f.calls.filter(c=>c.path?.endsWith('/stop')).length,1);
});

test('stop failure after submission clearly labels submitted state and keeps cancel unavailable',async()=>{
 const f=fixture({realControl:true,respond:path=>path.endsWith('/stop')?Promise.reject(new Error('timeout')):undefined});await f.ready();
 f.message({type:'conversation.item.input_audio_transcription.completed',transcript:'切到后勤组'});await tick();await tick();
 assert.equal(f.control.state.current_group_id,'group_b');assert.equal(f.voice.phase,'cleanup_pending');assert.equal(f.voice.canCancel,false);assert.match(f.voice.message,/请求已提交/);
});

test('legacy DataChannel open initializes once without waiting for session created and opens mic only on acknowledgement',async()=>{
 const f=fixture();await f.voice.initialize();await f.voice.start();
 assert.equal(f.voice.phase,'connecting');assert.equal(f.track.enabled,false);assert.equal(f.peer.channel.sent.length,0);
 f.peer.channel.onopen();assert.equal(f.voice.phase,'awaiting_session');assert.match(f.voice.message,/等待配置确认/);
 assert.equal(f.peer.channel.sent.length,1);assert.equal(f.peer.channel.sent[0].type,'session.update');assert.equal(f.track.enabled,false);
 f.message({type:'session.created'});f.peer.channel.onopen();assert.equal(f.peer.channel.sent.length,1);
 f.message({type:'session.updated'});assert.equal(f.voice.phase,'listening');assert.equal(f.track.enabled,true);
 f.voice.interrupt();await tick();assert.equal(f.peer.channel.onopen,null);
});

test('session created before the open callback also uses the single initializer',async()=>{
 const f=fixture();await f.voice.initialize();await f.voice.start();f.message({type:'session.created'});f.peer.channel.onopen();
 assert.equal(f.peer.channel.sent.length,1);assert.equal(f.track.enabled,false);f.voice.interrupt();await tick();
});

test('timeout clears READY status and late session updated cannot reopen ended capture',async()=>{
 const stop=deferred();const f=fixture({respond:path=>path.endsWith('/stop')?stop.promise:undefined});await f.ready();
 const late=f.peer.channel.onmessage;const timeout=[...f.timers.values()].find(t=>t.ms===30000);timeout.fn();
 assert.equal(f.track.enabled,false);assert.equal(f.track.stopped,true);assert.match(f.voice.message,/超时/);
 stop.resolve({stopped:true,command_id:'command_123'});await tick();assert.equal(f.voice.current,null);assert.equal(f.voice.phase,'idle');assert.match(f.voice.message,/超时/);
 late({data:JSON.stringify({type:'session.updated'})});assert.equal(f.track.enabled,false);assert.equal(f.voice.phase,'idle');assert.match(f.voice.message,/超时/);
});
