import test from 'node:test';
import assert from 'node:assert/strict';
import {ControlClient,bindAudioDisable} from './app.js';
import {VoiceCommandClient} from './voice.js';
import {RecommendationClient,voiceTargetReason} from './recommendation.js';
const deferred=()=>{let resolve;const promise=new Promise(done=>{resolve=done;});return{resolve,promise};};
const flush=async()=>{for(let i=0;i<30;i++)await Promise.resolve();};
function fixture(){
 const stop=deferred(),switched=deferred(),requests=[],pending=[];let peer;
 const state={device:{id:'device_123',name:'测试'},server_epoch:'epoch_123',server_time:new Date().toISOString(),current_group_id:'group_one',current_version:1,groups:[{id:'group_one',name:'全员群'},{id:'group_two',name:'后勤组'}]};
 const track={enabled:false,stopped:false,stop(){this.stopped=true;}};
 const stream={getAudioTracks:()=>[track],getTracks:()=>[track]};
 const audio={desired:true,phase:'ready',setSending(){},suspend(){},sync(){},disable(){this.desired=false;},isReady:()=>true};
 const control=new ControlClient({audio,onPending:value=>pending.push(value&&{...value}),fetchImpl:async(url,options)=>{
  const path=url.replace('/api/v1',''),body=options.body&&JSON.parse(options.body);requests.push({path,method:options.method,body});
  let result;
  if(path==='/voice')result={available:true,command_id:''};
  else if(path==='/media')result={peer_id:'old_group_peer'};
  else if(path==='/voice/sessions')result={command_id:body.command_id};
  else if(path.endsWith('/offer'))result={type:'answer',sdp:'v=0'};
  else if(path.endsWith('/stop'))result=await stop.promise;
  else if(path==='/voice/proposals')result={recommendation:{recommendation_id:'target_review_123',request_id:body.request_id,server_epoch:state.server_epoch,current_version:state.current_version,candidate_digest:'a'.repeat(64),expires_at:new Date(Date.now()+60000).toISOString(),source:'exact',suggestions:[{group_id:'group_two',name:'后勤组',purpose:'',reason:voiceTargetReason}]}};
  else if(path==='/recommendations/confirm')result=await switched.promise;
  else if(path==='/state')result=control.state;
  else result={closed:true};
  return{ok:true,status:200,json:async()=>result};
 }});
 control.identity={device:state.device,token:'synthetic-local-token'};control.state=state;control.connected=true;
 const voice=new VoiceCommandClient({control,audio,request:(...args)=>control.request(...args),random:()=> 'command_123',source:async()=>stream,createPeer:()=>{
  const channel={send(){},close(){}};peer={channel,connectionState:'connected',iceGatheringState:'complete',addTransceiver(){},createDataChannel:()=>channel,createOffer:async()=>({type:'offer',sdp:'v=0'}),async setLocalDescription(value){this.localDescription=value;},async setRemoteDescription(){},close(){this.connectionState='closed';}};return peer;
 }});control.voice=voice;control.recommendation=new RecommendationClient({control});
 const listeners={};const button={addEventListener(type,handler){listeners[type]=handler;},click(){listeners.click();}};bindAudioDisable(button,control);
 const ready=async()=>{await voice.initialize();await voice.start();peer.channel.onopen();peer.channel.onmessage({data:JSON.stringify({type:'session.updated'})});};
 const final=()=>peer.channel.onmessage({data:JSON.stringify({type:'conversation.item.input_audio_transcription.completed',transcript:'切到后勤组'})});
 return{voice,control,audio,button,track,stop,switched,requests,pending,ready,final,get peer(){return peer;}};
}

test('actual audio-off button stops a READY command synchronously before its HTTP cleanup',async()=>{
 const f=fixture();await f.ready();assert.equal(f.track.enabled,true);
 f.button.click();
 assert.equal(f.track.enabled,false);assert.equal(f.track.stopped,true);assert.equal(f.peer.connectionState,'closed');assert.equal(f.audio.desired,false);
 assert.equal(f.requests.some(r=>r.path.endsWith('/stop')),false);assert.equal(f.requests.some(r=>r.path==='/voice/resolve'||r.path==='/current-group'),false);
 await flush();assert.equal(f.voice.blocking,true);
 f.stop.resolve({stopped:true,command_id:'command_123'});await flush();assert.equal(f.voice.blocking,false);assert.equal(f.audio.desired,false);assert.equal(f.control.pendingSwitch,null);
});

test('actual audio-off button preserves a submitted durable switch and its in-flight confirmation',async()=>{
 const f=fixture();await f.ready();f.final();await flush();
 assert.equal(f.control.pendingSwitch,null);f.stop.resolve({stopped:true,command_id:'command_123'});await flush();
 const confirmation=f.control.recommendation.confirm('group_two');await flush();
 const operation={...f.control.pendingSwitch};assert.equal(operation.group_id,'group_two');assert.equal(f.voice.current,null);
 const epoch=f.control.epoch;f.button.click();assert.equal(f.control.epoch,epoch);assert.equal(f.track.stopped,true);assert.equal(f.audio.desired,false);assert.deepEqual(f.control.pendingSwitch,operation);
 assert.deepEqual(f.control.pendingSwitch,operation);
 f.switched.resolve({operation:{...operation,recommendation_epoch:operation.server_epoch,status:'applied',current_group_id:'group_two',current_version:2},state:{...f.control.state,current_group_id:'group_two',current_version:2}});await confirmation;await flush();
 assert.equal(f.control.pendingSwitch,null);assert.equal(f.control.state.current_group_id,'group_two');assert.equal(f.audio.desired,false);assert.equal(f.pending.at(-1),null);
});

test('actual audio-off button still releases an active PTT request',()=>{
 let interrupted=0,released=0,disabled=0;
 const client={pressed:true,floorRequest:{},voice:{interrupt(){interrupted++;}},cancelFloor(){released++;},audio:{disable(){disabled++;}}};
 let click;bindAudioDisable({addEventListener(type,handler){click=handler;}},client);click();assert.deepEqual([interrupted,released,disabled],[1,1,1]);
});
