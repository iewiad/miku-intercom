import test from 'node:test';
import assert from 'node:assert/strict';
import {pcmRMS, spectrumPeak, assessSamples, summarizeRTP, safeReport} from './diagnostics.js';

test('PCM RMS measures receiver waveform and silence', () => {
  const pcm = Float32Array.from({length: 4800}, (_, i) => 0.1 * Math.sin(2 * Math.PI * 440 * i / 48000));
  assert.ok(Math.abs(pcmRMS(pcm) - Math.SQRT1_2 * 0.1) < 0.00001);
  assert.equal(pcmRMS(new Float32Array(8192)), 0);
});
test('FFT peak uses the AudioContext actual sample rate', () => {
  const bins = new Float32Array(4096).fill(-Infinity); bins[75] = -25;
  assert.deepEqual(spectrumPeak(bins, 48000, 8192), {frequency: 439.453125, db: -25});
});
test('silence is not a pass when a remote receiver never produced valid samples', () => {
  assert.equal(assessSamples([], null).pass, false);
  assert.equal(assessSamples([{valid: false, rms: 0, frequency: 0}], null).pass, false);
  assert.equal(assessSamples(Array.from({length: 10}, () => ({valid: true, rms: 0, frequency: 0})), null).pass, true);
});
test('tone requires both decoded amplitude and the expected frequency', () => {
  const tone = Array.from({length: 10}, () => ({valid: true, rms: 0.07, frequency: 440}));
  assert.equal(assessSamples(tone, 440).pass, true);
  assert.equal(assessSamples(tone, 997).pass, false);
  assert.equal(assessSamples(tone, null).pass, false);
});
test('RTP diagnostics select only inbound audio and resolve codec IDs', () => {
  const stats = new Map([['codec', {mimeType: 'audio/opus', clockRate: 48000}],
    ['in', {type: 'inbound-rtp', kind: 'audio', codecId: 'codec', packetsReceived: 50, bytesReceived: 1234}],
    ['out', {type: 'outbound-rtp', kind: 'audio', packetsSent: 999}]]);
  assert.deepEqual(summarizeRTP(stats), [{packetsReceived: 50, bytesReceived: 1234, totalAudioEnergy: null,
    totalSamplesDuration: null, codec: 'audio/opus', clockRate: 48000}]);
});
test('report export does not copy identity credentials, SDP or Peer objects', () => {
  const report = safeReport({mode: 'single-independent-tab', status: 'observations-only', results: [],
    token: 'never-export', sdp: 'never-export', identity: {token: 'never-export'}, peer: {}});
  assert.equal(JSON.stringify(report).includes('never-export'), false);
  assert.equal('identity' in report, false); assert.equal('peer' in report, false);
});
