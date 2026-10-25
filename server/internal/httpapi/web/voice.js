import {validNeed, validVoiceTarget} from './recommendation.js';

const finalTypes = new Set(['conversation.item.input_audio_transcription.completed', 'conversation.item.input_audio_transcription.done']);
const sourceFactory = () => navigator.mediaDevices.getUserMedia({audio:{channelCount:{ideal:1},echoCancellation:true,noiseSuppression:true,autoGainControl:true},video:false});
const stopTracks = stream => { for (const track of stream?.getTracks?.() || []) { track.enabled=false; track.stop(); } };
export const voiceErrorText = error => ({
  voice_unavailable:'服务尚未启用语音切群。',
  voice_target_unknown:'本次目标不在已加入群中，未切群，语音指令已结束。请重新点击或用群列表选择。',
  voice_target_ambiguous:'本次目标群不明确，未切群，语音指令已结束。请用群列表选择。',
  invalid_voice_command:'本次未识别到切群指令，已结束。重新点击后请说“切到”加完整群名。',
  voice_cleanup_unconfirmed:'语音会话结束尚未确认，正在重试；对讲暂时暂停。',
}[error?.code] || (error?.name==='NotAllowedError' ? '麦克风权限未获允许。' : '语音操作未完成，正在安全恢复。'));

// One command owns the microphone and the only local Peer until upstream stop is acknowledged.
export class VoiceCommandClient {
  constructor({control, audio, request, scopeRequest=()=>request, source=sourceFactory,
    createPeer=()=>new RTCPeerConnection({iceServers:[]}), random=()=>crypto.randomUUID(),
    schedule=(fn,delay)=>setTimeout(fn,delay), cancel=id=>clearTimeout(id), isVisible=()=>typeof document==='undefined'||!document.hidden, onChange=()=>{}}={}) {
    Object.assign(this,{control,audio,request,scopeRequest,source,createPeer,random,schedule,cancel,isVisible,onChange});
    this.phase='checking'; this.available=false; this.current=null; this.generation=0; this.message='正在检查语音切群。';
    this.initializing=null;
  }
  get blocking() { return !!this.current || this.phase==='checking'; }
  get canCancel() { return !!this.current && !this.current.finishing; }
  emit(){this.onChange(this);}
  async initialize() {
    if(this.initializing) return this.initializing;
    if(this.current) return;
    this.phase='checking';this.emit();
    const request=this.scopeRequest();
    this.initializing=(async()=>{
      try {
        const status=await request('/voice');
        this.available=!!status.available;
        if(status.command_id){
          const session={id:status.command_id,request,generation:++this.generation,createRequested:true};
          this.current=session;await this.finish(session,'已结束上次语音操作。');return;
        }
        this.phase=this.available?'idle':'unavailable';this.message=this.available?'说“切到”加完整群名，识别后核对确认；期间暂时无法收听群聊。':'服务尚未启用语音切群。';
      } catch {this.phase='checking';this.message='语音会话状态未确认，请重试检查。';}
      finally {this.emit();if(!this.blocking)void this.audio.sync();}
    })();
    try{await this.initializing;}finally{this.initializing=null;this.emit();}
  }
  currentSession(s){
    if(this.current!==s||s.generation!==this.generation||s.finishing)return false;
    if(!this.isVisible()||!this.control.allowed()||s.identityToken!==this.control.identity?.token||
       this.control.recommendation?.voiceReservation!==s.reservation){
      this.interrupt('页面或身份状态已变化，语音采集已停止。');return false;
    }
    return true;
  }
  async start({mode='command'}={}){
    if(!['command','need'].includes(mode) || !this.isVisible() || this.blocking || !this.available || !this.control.allowed() || !this.control.connected || this.control.uiBlocked || this.control.recommendation?.blocking || this.control.pendingSwitch || this.control.mutationBusy || !this.control.state?.current_group_id)return;
    const reservation=this.control.recommendation?.reserveVoice(mode==='need'?'need':'target');
    if(!reservation)return;
    const s={id:this.random(),request:this.scopeRequest(),generation:++this.generation,mode,reservation,identityToken:this.control.identity?.token,createRequested:false,accepted:false,cancelled:false};
    this.current=s;this.phase='preparing';this.message='正在关闭群音频，准备语音指令。';
    this.control.cancelFloor();this.audio.suspend('语音指令期间群音频暂停，结束后恢复。');this.emit();
    s.timer=this.schedule(()=>{if(this.currentSession(s))void this.finish(s,'语音指令超时，已停止采集。');},30000);
    try{
      // Confirm server-side group Peer deletion as well as synchronous local close.
      const media=await s.request('/media');if(!this.currentSession(s))return;
      if(media.peer_id)await s.request(`/media/peers/${encodeURIComponent(media.peer_id)}`,'DELETE');
      if(!this.currentSession(s))return;
      this.phase='creating';this.message='正在创建语音会话，暂时无法收听群聊。';this.emit();
      s.createRequested=true;
      const created=await s.request('/voice/sessions','POST',{command_id:s.id});
      if(!this.currentSession(s))return;
      if(created.command_id!==s.id)throw new Error('invalid command session');
      this.phase='capture';this.message='正在准备麦克风，请留意浏览器权限提示。';this.emit();
      const stream=await this.source();
      for(const track of stream.getAudioTracks())track.enabled=false;
      if(!this.currentSession(s)){stopTracks(stream);return;}
      s.stream=stream;const track=stream.getAudioTracks()[0];if(!track)throw new Error('no microphone');
      const peer=this.createPeer();s.peer=peer;
      peer.addTransceiver(track,{direction:'sendrecv',streams:[stream]});
      peer.ontrack=event=>{event.track.enabled=false;}; // Command sessions never play assistant audio.
      peer.onconnectionstatechange=()=>{
        if(!this.currentSession(s))return;
        if(['failed','closed','disconnected'].includes(peer.connectionState))void this.finish(s,'语音连接中断，正在恢复。');
        else if(peer.connectionState==='connected'&&!s.updated){this.phase='connecting';this.message='语音已连接，正在准备识别。';this.emit();}
      };
      const channel=peer.createDataChannel('oai-events',{ordered:true});s.channel=channel;
      channel.onopen=()=>this.ensureSession(s);
      channel.onmessage=event=>this.receive(s,event.data);
      channel.onclose=()=>{if(this.currentSession(s))void this.finish(s,'语音连接已关闭，正在恢复。');};
      channel.onerror=()=>{if(this.currentSession(s))void this.finish(s,'语音连接失败，正在恢复。');};
      track.onended=()=>{if(this.currentSession(s)&&!s.accepted)void this.finish(s,'麦克风已停止，正在恢复。');};
      this.phase='ice';this.message='正在准备语音连接，请稍候。';this.emit();
      await peer.setLocalDescription(await peer.createOffer());
      await this.waitICE(s);if(!this.currentSession(s))return;
      this.phase='connecting';this.message='正在连接语音服务。';this.emit();
      const answer=await s.request(`/voice/sessions/${encodeURIComponent(s.id)}/offer`,'POST',{type:'offer',sdp:peer.localDescription.sdp});
      if(!this.currentSession(s))return;
      if(answer.type!=='answer'||typeof answer.sdp!=='string')throw new Error('invalid RTA answer');
      await peer.setRemoteDescription(answer);
      if(this.currentSession(s)&&!s.updated){this.phase='connecting';this.message='正在连接语音识别，请稍候。';this.emit();}
    }catch(error){if(this.currentSession(s))await this.finish(s,voiceErrorText(error));}
  }
  waitICE(s){
    if(s.peer.iceGatheringState==='complete')return Promise.resolve();
    return new Promise((resolve,reject)=>{
      let timer;
      const done=()=>{this.cancel(timer);s.peer.removeEventListener('icegatheringstatechange',changed);s.iceCancel=null;};
      const changed=()=>{if(s.peer.iceGatheringState==='complete'){done();resolve();}};
      s.iceCancel=()=>{done();reject(new Error('cancelled'));};
      s.peer.addEventListener('icegatheringstatechange',changed);
      timer=this.schedule(()=>{done();reject(new Error('ICE timeout'));},5000);changed();
    });
  }
  ensureSession(s){
    if(!this.currentSession(s)||s.accepted||s.updated)return;
    // The existing legacy route is initialized when oai-events opens; it need not
    // send session.created first. Both signals share this one-shot initializer.
    s.updated=true;this.phase='awaiting_session';this.message='语音通道已连接，等待配置确认。';this.emit();
    try{s.channel.send(JSON.stringify({type:'session.update',session:{modalities:['text','audio'],instructions:'仅转写用户本次语音，不回答，不调用工具。',input_audio_format:'pcm',output_audio_format:'pcm',input_audio_transcription:{model:'qwen3-asr-flash-realtime'},turn_detection:{type:'semantic_vad',threshold:0.5,silence_duration_ms:1100,create_response:false,interrupt_response:false}}}));}catch{void this.finish(s,'语音初始化失败，正在恢复。');}
  }
  receive(s,raw){
    if(!this.currentSession(s)||s.accepted||typeof raw!=='string'||raw.length>32768)return;
    let event;try{event=JSON.parse(raw);}catch{void this.finish(s,'语音事件无效，正在恢复。');return;}
    if(event.type==='session.created'){
      this.ensureSession(s);
    }else if(event.type==='session.updated'&&s.updated){
      this.phase='listening';this.message=s.mode==='need'?'请描述需要的帮助，结束后会生成建议，由你确认进入。':'请说“切到”加完整群名，识别后核对确认。';for(const track of s.stream?.getAudioTracks()||[])track.enabled=true;this.emit();
    }else if(finalTypes.has(event.type)&&this.phase==='listening'){
      if(s.mode==='need'?!validNeed(event.transcript):!validVoiceTarget(event.transcript)){void this.finish(s,s.mode==='need'?'需求内容无效，请重新说出 1–512 个字符的需求。':'没有识别到有效群名指令，正在恢复。');return;}
      s.accepted=true;for(const track of s.stream?.getAudioTracks()||[])track.enabled=false;
      s.need=event.transcript;this.control.recommendation.awaitVoiceCleanup(s.reservation);
      void this.finish(s,s.mode==='need'?'已停止采集，正在结束语音会话后获取建议。':'已停止采集，正在结束语音会话后核对目标群。');
    }else if(event.type==='session.closed'||event.type==='error'||event.type==='rtid.error'||event.type==='conversation.item.input_audio_transcription.failed')void this.finish(s,'语音会话未完成，正在恢复。');
  }
  interrupt(message='已取消语音指令。'){
    const s=this.current;if(!s)return;
    s.cancelled=true;s.need=null;s.cancelMessage=message;
    if(this.control.recommendation?.voiceReservation===s.reservation)this.control.recommendation.interrupt(message);
    // This owner only captures a proposal; confirmation belongs to the control client.
    void this.finish(s,message);
  }
  interruptReservation(reservation,message){if(this.current?.reservation===reservation)this.interrupt(message);}
  reservationActive(s){
    const c=this.control;
    return this.current===s&&!s.cancelled&&this.isVisible()&&c.allowed()&&
      s.identityToken===c.identity?.token&&!c.uiBlocked&&!c.pendingSwitch&&!c.mutationBusy&&
      c.recommendation?.voiceReservation===s.reservation;
  }
  canHandoffVoice(reservation){
    const s=this.current;
    return s?.reservation===reservation&&s.cleanConfirmed===true&&s.authorizedStateRefreshed===true&&this.reservationActive(s);
  }
  async finish(s,message){
    if(s.finishTask)return s.finishTask;
    s.finishing=true;this.cancel(s.timer);this.cancel(s.retry);s.iceCancel?.();
    this.phase='cleaning';this.message=message;this.emit();
    let closed=true;
      try{stopTracks(s.stream);if(s.channel){s.channel.onopen=s.channel.onmessage=s.channel.onclose=s.channel.onerror=null;s.channel.close();}if(s.peer){s.peer.ontrack=s.peer.onconnectionstatechange=null;s.peer.close();closed=s.peer.connectionState==='closed';}}catch{closed=false;}
    // Defer the task body so a synchronous cleanup error cannot overwrite the retry reset.
    s.finishTask=Promise.resolve().then(async()=>{
      try{
        if(s.createRequested&&!s.stopConfirmed){const stopped=await s.request(`/voice/sessions/${encodeURIComponent(s.id)}/stop`,'POST',undefined,{keepalive:true});if(stopped.stopped!==true||stopped.command_id!==s.id)throw new Error('stop unconfirmed');s.stopConfirmed=true;}
        if(!closed)throw new Error('local Peer close unconfirmed');
      }catch{
        this.phase='cleanup_pending';this.message='语音会话结束尚未确认，正在重试；对讲暂时暂停。';this.emit();
        s.finishTask=null;s.retry=this.schedule(()=>{void this.finish(s,message);},2000);return;
      }
      if(this.current!==s)return;
      let finalMessage=message;
      if(s.need&&this.reservationActive(s)){
        s.cleanConfirmed=true;this.phase=s.mode==='need'?'refreshing_need':'refreshing_target';this.message=s.mode==='need'?'语音会话已结束，正在核对最新群组后获取建议。':'语音会话已结束，正在核对最新群组和语音目标。';this.emit();
        try{
          const epoch=this.control.epoch,next=await s.request('/state');
          if(this.reservationActive(s)){
            if(!this.control.applyState(next,epoch))throw new Error('state unconfirmed');
            s.authorizedStateRefreshed=true;
            const need=s.need;s.need=null;
            // request() admits the verified reservation synchronously before its
            // first await, so there is no gap that re-enables PTT or another task.
            if(s.mode==='need')void this.control.recommendation.request(need,s.reservation);
            else void this.control.recommendation.requestVoiceTarget(need,s.reservation);
            finalMessage=s.mode==='need'?'语音会话已结束。请核对需求建议，并明确确认要进入的群。':'语音会话已结束。请核对识别目标群，确认后才会切换。';
          }
        }catch{
          if(this.reservationActive(s)){
            this.control.connected=false;
            finalMessage='群状态尚未确认，未请求推荐；请恢复连接后重试。';
          }
        }
      }
      s.need=null;
      if(this.control.recommendation?.voiceReservation===s.reservation)this.control.recommendation.interrupt(s.cancelled?'已取消本次语音操作，当前群保持不变。':finalMessage);
      if(s.cancelled&&s.cancelMessage)finalMessage=s.cancelMessage;
      this.current=null;++this.generation;this.phase=this.available?'idle':'unavailable';this.message=finalMessage;this.emit();
      // Latest control state (or its durable pending operation) decides which group may resume.
      if(this.control.allowed()){if(!this.control.pendingSwitch&&!s.authorizedStateRefreshed)await this.control.refresh();void this.audio.sync();}
    });
    return s.finishTask;
  }
}
