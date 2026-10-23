// Run with node or macOS JavaScriptCore from the repository root.
'use strict';
const path = 'cmd/rtid/examples/lingxi-realtime-browser/index.html';
const html = typeof readFile === 'function' ? readFile(path) : require('fs').readFileSync(path,'utf8');
const elements = new Map();
const timers = new Map();
let timerID = 0;
const documentMock = {getElementById(id) {
  if (!elements.has(id)) elements.set(id,{value:id==='transport'?'wss':'',textContent:'',options:[{},{}],replaceChildren(){this.options=[];this.value='';},add(option){this.options.push(option);if(this.options.length===1)this.value=option.value;},pause(){}});
  return elements.get(id);
}};
const tests = `
function assert(condition,message) { if(!condition) throw new Error(message); }
void loadVoices();
assert(ui.voice.value==='default_voice','voice refresh defaults to default_voice');
ui.rticBaseURL.value='http://127.0.0.1:19190';
ui.rticToken.value='test-token';
ui.appID.value='test-app';
ui.voice.value='';
controls();
assert(!ui.connect.disabled,'empty voice must not block connection');
ui.appID.value='';
controls();
assert(ui.connect.disabled,'RTIC configuration remains required');
const failureCall={socketEndpoint:'wss://voice.example.test/v1/realtime/sessions/session-1/ws'};
assert((await websocketFailure(failureCall,{code:0},'error')).includes('TLS/网络'),'WSS error must mention TLS diagnosis');
assert((await websocketFailure(failureCall,{code:1006,reason:''},'close')).includes('网络不可达'),'abnormal WSS close must mention network diagnosis');
assert((await websocketFailure(failureCall,{code:1008,reason:'policy'},'close')).includes('服务端策略拒绝'),'policy close must expose diagnosis');
const originalFetch=fetch;
let diagnosisRequests=0,resolveDiagnosis;
fetch=()=>{diagnosisRequests++;return new Promise(resolve=>{resolveDiagnosis=resolve;});};
const diagnosticCall={generation:++generation,socketURL:'wss://voice.example.test/v1/realtime/sessions/session-1/ws?access_token=test',sessionID:'session-1',accessToken:'test'};
call=diagnosticCall;
const displayedDiagnostics=[];
function displayDiagnosis(message) {
  if (!current(diagnosticCall)) return;
  displayedDiagnostics.push(message);
  showSessionError(message);
  call=null;
}
const errorDiagnosis=websocketFailure(diagnosticCall,{},'error').then(displayDiagnosis);
const closeDiagnosis=websocketFailure(diagnosticCall,{code:1006},'close').then(displayDiagnosis);
await Promise.resolve();
assert(diagnosisRequests===1&&call===diagnosticCall&&displayedDiagnostics.length===0,'error and close must share the pending diagnosis without early cleanup');
resolveDiagnosis({json:async()=>({phase:'http_upgrade',http_status:401,body:'access_token_invalid'})});
await Promise.all([errorDiagnosis,closeDiagnosis]);
assert(displayedDiagnostics.length===1&&ui.sessionError.textContent.includes('401')&&ui.sessionError.textContent.includes('access_token_invalid')&&!ui.sessionError.hidden,'diagnosis response must remain visible after concurrent error and close');
call={generation:++generation};
const previousMessage=ui.sessionError.textContent;
await websocketFailure(diagnosticCall,{code:1006},'close').then(displayDiagnosis);
assert(ui.sessionError.textContent===previousMessage&&call!==null,'stale diagnosis must not affect a new session');
fetch=originalFetch;
const sent=[];
const c={generation:++generation,transport:'wss',mode:'manual',ready:true,sources:new Set(),tail:new Float32Array(),position:0,inputRate:16000,outputRate:24000,socket:{readyState:1,bufferedAmount:0,send(value){sent.push(JSON.parse(value));}},audio:{currentTime:0}};
call=c;
receive(c,JSON.stringify({type:'session.created'}));
assert(sent[0].type==='session.update'&&sent[0].session.audio.input.turn_detection===null,'manual initialization');
capturePCM(c,new Float32Array([1,-1,0,0]),16000);
assert(sent[1].type==='input_audio_buffer.append'&&sent[1].audio==='/38AgAAA','PCM16 little endian');
assert(c.hasInput,'input tracked');
commit(c);
assert(sent[2].type==='input_audio_buffer.commit'&&sent[3].type==='response.create','manual turn');
receive(c,JSON.stringify({type:'response.created',response:{id:'r1'}}));
receive(c,JSON.stringify({type:'response.text.delta',response_id:'r1',delta:'hello'}));
receive(c,JSON.stringify({type:'response.output_audio_transcript.delta',response_id:'r1',delta:'hello'}));
assert(ui.assistantSubtitle.textContent==='hello','text and transcript must not duplicate');
receive(c,JSON.stringify({type:'conversation.item.input_audio_transcription.completed',transcript:'user'}));
assert(ui.userSubtitle.textContent==='user','user subtitle');
for (const enabled of [true,false]) {
  for (const transport of ['rtc','wss']) {
    const autoCall={...c,transport,mode:'auto',updated:false,interruptResponse:enabled,inputTranscription:enabled,sources:new Set()};
    call=autoCall;
    receive(autoCall,JSON.stringify({type:'session.created'}));
    const input=sent[sent.length-1].session.audio.input;
    assert(input.turn_detection.interrupt_response===enabled,transport+' interrupt setting follows checkbox');
    assert(enabled ? JSON.stringify(input.transcription)==='{}' : input.transcription===null,transport+' transcription uses GA object/null switch');
    let automaticStops=0;
    autoCall.sources.add({stop(){automaticStops++;}});
    const beforeSpeech=sent.length;
    receive(autoCall,JSON.stringify({type:'input_audio_buffer.speech_started'}));
    assert(automaticStops===(enabled?1:0),transport+' automatic playback interruption follows checkbox');
    assert(sent.length===beforeSpeech,transport+' automatic interruption is owned by Provider, without duplicate cancel');
  }
}
call=null;
ui.userSubtitle.textContent='user';
ui.transport.value='wss';ui.turn.value='auto';controls();
assert(!ui.inputTranscription.disabled,'WSS transcription checkbox is now enabled');
assert(!ui.interruptResponse.disabled,'WSS interrupt checkbox is enabled for auto');
ui.turn.value='manual';controls();
assert(ui.interruptResponse.disabled,'manual turn disables interrupt checkbox on all transports');
call=c;
interrupt(c);interrupt(c);
assert(sent.filter(event=>event.type==='response.cancel').length===1&&c.suppressPlayback,'single cancel and stop playback');
const cancelTimeout = timers.get(c.cancelTimer);
receive(c,JSON.stringify({type:'error',error:{code:'response_cancel_not_active',message:'private provider detail'}}));
assert(call===c&&c.cancelPending&&c.responseID==='r1','cancel request error must not disconnect or invent a terminal response');
assert(ui.sessionError.textContent.includes('response_cancel_not_active')&&!ui.sessionError.hidden,'safe error code is visible');
assert(!ui.sessionError.textContent.includes('private provider detail')&&!ui.events.textContent.includes('private provider detail'),'raw error details remain private');
cancelTimeout();
assert(call===c&&c.cancelPending&&ui.sessionError.textContent.includes('连接保留'),'cancel timeout keeps session and response state');
receive(c,JSON.stringify({type:'response.text.delta',response_id:'r1',delta:'late'}));
assert(ui.assistantSubtitle.textContent==='hello','late text suppressed');
receive(c,JSON.stringify({type:'response.done',response:{id:'r1',status:'cancelled'}}));
assert(!c.cancelPending&&!c.responseActive&&!c.responseID,'terminal response lifecycle');
receive(c,JSON.stringify({type:'response.created',response:{id:'r2'}}));
let stoppedSources=0;
c.sources.add({stop(){stoppedSources++;}});
receive(c,JSON.stringify({type:'response.done',response:{id:'r2',status:'completed'}}));
assert(!ui.interrupt.disabled,'queued playback remains interruptible after response.done');
const cancelsBefore=sent.filter(event=>event.type==='response.cancel').length;
interrupt(c);
assert(stoppedSources===1&&c.sources.size===0,'interrupt stops queued audio immediately');
assert(sent.filter(event=>event.type==='response.cancel').length===cancelsBefore,'completed response must not send idle cancel');
receive(c,JSON.stringify({type:'response.created',response:{id:'r2'}}));
c.audio.createBuffer=()=>({duration:1,getChannelData:()=>new Float32Array(1)});
c.audio.createBufferSource=()=>({connect(){},start(){},stop(){stoppedSources++;}});
c.nextPlay=299;
receive(c,JSON.stringify({type:'response.audio.delta',response_id:'r2',delta:'AAA='}));
assert(c.nextPlay===300&&c.sources.size===1&&!c.suppressPlayback,'exactly five minutes of queued playback is allowed');
c.sources.add({stop(){stoppedSources++;}});
playPCMGuardTest();
function playPCMGuardTest() {
  receive(c,JSON.stringify({type:'response.audio.delta',response_id:'r2',delta:'AAA='}));
  assert(call===c&&c.sources.size===0&&c.suppressPlayback,'queue overflow clears playback without closing the session');
  assert(ui.events.textContent.includes('queue_overflow'),'overflow gets a distinct safe reason');
  assert(ui.sessionError.textContent.includes('5 分钟'),'overflow message uses the five-minute limit');
  assert(ui.sessionError.textContent.includes('连接保留'),'playback recovery is visible');
}
receive(c,JSON.stringify({type:'response.done',response:{id:'r2',status:'completed'}}));
receive(c,JSON.stringify({type:'response.created',response:{id:'r3'}}));
receive(c,JSON.stringify({type:'response.audio.delta',response_id:'r3'}));
assert(call===c&&c.suppressPlayback&&ui.events.textContent.includes('invalid_audio_or_device'),'invalid audio is local to playback');
receive(c,JSON.stringify({type:'response.done',response:{id:'r3',status:'completed'}}));
receive(c,JSON.stringify({type:'response.created',response:{id:'r2'}}));
assert(!c.suppressPlayback,'next response can play after local audio failure');
showSessionError('');
cancelTimeout();
assert(ui.sessionError.hidden&&c.responseID==='r2','old cancel timeout cannot affect the next response');
receive(c,JSON.stringify({type:'error',error:{code:'invalid_request_error'}}));
assert(call===c&&c.responseID==='r2','nonterminal provider errors retain the session');
const failures=[];
const originalFail=fail;
fail=(context,message,closeCode)=>failures.push({context,message,closeCode});
receive(c,JSON.stringify({type:'error',error:{code:'rti_session_terminated',terminal:true}}));
assert(failures.length===1&&failures[0].context===c,'explicit terminal errors still fail the session');
assert(failures[0].closeCode===4001,'terminal provider errors have a distinct close code');
fail=originalFail;
let clientCloseCode;
c.abort={abort(){}};
c.audio.state='closed';
c.socket.close=code=>{clientCloseCode=code;};
receive(c,'invalid JSON');
assert(call===null&&clientCloseCode===4004,'client failures close with their diagnostic code instead of an empty frame');
assert(ui.events.textContent.includes('session.error code=4004'),'client failure code remains in page event log');
call=null;
receive(c,JSON.stringify({type:'conversation.item.input_audio_transcription.completed',transcript:'stale'}));
assert(ui.userSubtitle.textContent==='user','stale callback ignored');
`;
const source = html.match(/<script>([\s\S]*?)<\/script>/)[1].replace('void loadVoices();','');
if (!html.includes('<option value="default_voice">default_voice</option>')) throw new Error('initial voice must be default_voice');
new Function('document','window','WebSocket','fetch','setTimeout','clearTimeout','Option','atob','btoa','timers',source+'\nreturn (async()=>{'+tests+'})()')(
 documentMock,{addEventListener(){}},{OPEN:1},()=>{},callback=>{timers.set(++timerID,callback);return timerID;},id=>timers.delete(id),
 function Option(text,value){this.text=text;this.value=value;},
 value=>{
   if(typeof Buffer!=='undefined') return Buffer.from(value,'base64').toString('binary');
   const alphabet='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';let bits=0,count=0,out='';
   for(const character of value.replace(/=+$/,'')){bits=(bits<<6)|alphabet.indexOf(character);count+=6;if(count>=8){count-=8;out+=String.fromCharCode((bits>>count)&255);}}
   return out;
 },
 value=>{
   if(typeof Buffer!=='undefined') return Buffer.from(value,'binary').toString('base64');
   const alphabet='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';let out='';
   for(let i=0;i<value.length;i+=3){const n=(value.charCodeAt(i)<<16)|((value.charCodeAt(i+1)||0)<<8)|(value.charCodeAt(i+2)||0);out+=alphabet[(n>>18)&63]+alphabet[(n>>12)&63]+(i+1<value.length?alphabet[(n>>6)&63]:'=')+(i+2<value.length?alphabet[n&63]:'=');}
   return out;
 },timers
).then(()=>{
  if(typeof print==='function') print('Lingxi browser syntax and protocol tests: passed');else console.log('Lingxi browser syntax and protocol tests: passed');
}).catch(error=>{
  if(typeof process!=='undefined') { console.error(error);process.exitCode=1; }
  else { throw error; }
});
