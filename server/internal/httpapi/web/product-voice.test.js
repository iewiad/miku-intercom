import test from 'node:test';
import assert from 'node:assert/strict';
const flush=async()=>{for(let i=0;i<100;i++)await Promise.resolve();};
test('actual boot/render and audio-off button replace READY status after terminal cleanup',async()=>{
 const originals=new Map();const install=(key,value)=>{originals.set(key,Object.getOwnPropertyDescriptor(globalThis,key));Object.defineProperty(globalThis,key,{value,writable:true,configurable:true});};
 const nodes=new Map();class Element{
  constructor(){this.textContent='';this.dataset={};this.listeners={};this.value='';this.srcObject=null;}
  addEventListener(type,fn){(this.listeners[type]??=[]).push(fn);}dispatch(type){for(const fn of this.listeners[type]||[])fn({preventDefault(){}});}
  replaceChildren(){}append(){}setAttribute(){}setCustomValidity(){}focus(){}pause(){}play(){return Promise.resolve();}
 }
 const $=id=>{if(!nodes.has(id))nodes.set(id,new Element());return nodes.get(id);};
 const store=new Map();const identity={device:{id:'device_test',name:'测试'},token:'synthetic-local-token'};
 store.set('shenglu.control.identity.v1',JSON.stringify(identity));
 const state={device:identity.device,server_epoch:'epoch_test',server_time:new Date().toISOString(),current_group_id:'group_test',current_version:1,groups:[{id:'group_test',name:'测试群',member_count:1,floor:null}]};
 const streams=[],peers=[];let media=null,commandID;
 const source=()=>{const track={enabled:true,kind:'audio',stop(){this.enabled=false;}};const stream={getTracks:()=>[track],getAudioTracks:()=>[track]};streams.push(stream);return Promise.resolve(stream);};
 class Peer{
  constructor(){this.connectionState='connected';this.iceGatheringState='complete';peers.push(this);}
  addTransceiver(){return{setCodecPreferences(){}};}createOffer(){return Promise.resolve({type:'offer',sdp:'v=0'});}setLocalDescription(value){this.localDescription=value;return Promise.resolve();}
  createDataChannel(){this.channel={send(){},close(){}};return this.channel;}
  async setRemoteDescription(){if(this.channel){this.channel.onopen();this.channel.onmessage({data:JSON.stringify({type:'session.updated'})});}else{const track={enabled:true,kind:'audio'};await this.ontrack({track,streams:[{getTracks:()=>[track]}]});}}
  close(){this.connectionState='closed';}
 }
 try{
  install('document',{hidden:false,getElementById:$,createElement:()=>new Element(),addEventListener(){}});
  install('window',{addEventListener(){}});install('localStorage',{getItem:key=>store.get(key)||null,setItem:(key,value)=>store.set(key,value),removeItem:key=>store.delete(key)});
  install('navigator',{mediaDevices:{getUserMedia:source}});install('RTCPeerConnection',Peer);install('RTCRtpReceiver',{getCapabilities:()=>({codecs:[{mimeType:'audio/opus'}]})});install('setInterval',()=>1);
  install('fetch',async(url,options={})=>{
   const path=url.replace('/api/v1',''),body=options.body&&JSON.parse(options.body);let result;
   if(path==='/voice')result={available:true,command_id:''};else if(path==='/state')result=state;
   else if(path==='/media')result={available:true,server_epoch:'epoch_test',peer_id:media?.peer_id||''};
   else if(path==='/media/peers'&&options.method==='POST'){media=body;result={...body,answer:{type:'answer',sdp:'v=0'}};}
   else if(path.startsWith('/media/peers/')&&options.method==='DELETE'){media=null;result={closed:true};}
   else if(path==='/voice/sessions'){commandID=body.command_id;result={command_id:commandID};}
   else if(path.endsWith('/offer'))result={type:'answer',sdp:'v=0'};
   else if(path.endsWith('/stop'))result={stopped:true,command_id:commandID};else if(path==='/voice/resolve')return{ok:false,status:400,json:async()=>({error:{code:'invalid_voice_command'}})};else throw new Error('unexpected product request');
   return{ok:true,status:200,json:async()=>result};
  });
  await import(`./app.js?product-render-review=${Date.now()}`);await flush();
  $('audio-enable').dispatch('click');await flush();assert.equal($('audio-status').dataset.phase,'ready');
  $('voice-start').dispatch('click');await flush();assert.match($('voice-status').textContent,/请说/);assert.equal(streams.at(-1).getAudioTracks()[0].enabled,true);
  assert.equal($('add-group-open').disabled,true);$('add-group-open').dispatch('click');assert.equal($('add-group-panel').hidden,true);
  $('audio-disable').dispatch('click');assert.equal(streams.at(-1).getAudioTracks()[0].enabled,false);assert.doesNotMatch($('voice-status').textContent,/请说/);
  await flush();assert.equal($('voice-start').disabled,false);assert.equal($('audio-enable').hidden,false);assert.match($('voice-status').textContent,/已关闭/);assert.doesNotMatch($('voice-status').textContent,/请说/);
  // Ambient speech can finish a real ASR turn without being a command. Its
  // rejected final must not leave the same invitation as a live READY session.
  $('voice-start').dispatch('click');await flush();assert.equal($('voice-status').textContent,'请说“切到”加完整群名。');
  peers.at(-1).channel.onmessage({data:JSON.stringify({type:'conversation.item.input_audio_transcription.completed',transcript:'背景输入'})});await flush();
  assert.equal($('voice-start').disabled,false);assert.match($('voice-status').textContent,/未识别.*已结束/);assert.notEqual($('voice-status').textContent,'请说“切到”加完整群名。');assert.equal(streams.at(-1).getAudioTracks()[0].enabled,false);

 }finally{for(const [key,descriptor]of originals){if(descriptor)Object.defineProperty(globalThis,key,descriptor);else delete globalThis[key];}}
});
