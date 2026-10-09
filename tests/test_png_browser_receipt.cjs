/* Offline DOM/PNG receipt integrity tests. Synthetic image bytes only. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto');
const zlib = require('node:zlib');
const SOURCE = fs.readFileSync(path.join(__dirname, 'receipt.js'), 'utf8');
const FILES = process.env.XRDP_CONSOLE_TEST_PNG_FIXTURE_DIR ||
  path.join(__dirname, 'generated');
const crcTable = Array.from({length:256}, (_,i) => {
  let c=i;for(let j=0;j<8;j++)c=(c&1)?(0xedb88320^(c>>>1)):(c>>>1);return c>>>0;
});
function crc32(bytes) {
  let c=0xffffffff;
  for(const b of bytes)c=crcTable[(c^b)&255]^(c>>>8);
  return (c^0xffffffff)>>>0;
}
function fullDecode(png) {
  if (!png.subarray(0,8).equals(Buffer.from('89504e470d0a1a0a','hex')))
    throw Error('PNG signature');
  let p=8,width=0,height=0,channels=0,idat=[];
  while (p<png.length) {
    const n=png.readUInt32BE(p), kind=png.toString('ascii',p+4,p+8);
    if (p+12+n>png.length)throw Error('truncation');
    if (crc32(png.subarray(p+4,p+8+n))!==png.readUInt32BE(p+8+n))
      throw Error('CRC mismatch');
    if (kind==='IHDR') {
      width=png.readUInt32BE(p+8);height=png.readUInt32BE(p+12);
      channels=png[p+17]===2?3:png[p+17]===6?4:0;
    }
    if (kind==='IDAT')idat.push(png.subarray(p+8,p+8+n));
    p+=12+n;
    if (kind==='IEND')break;
  }
  if (p!==png.length || !channels || width===0 || height===0)
    throw Error('bad PNG chunks');
  const raw=zlib.inflateSync(Buffer.concat(idat));
  if (raw.length!==height*(1+width*channels))
    throw Error('incorrect decompressed PNG bytes');
  return {width,height};
}
function testPage({subtle=crypto.webcrypto.subtle,decoder=fullDecode}={}) {
  const events=new Map(),window={addEventListener(name,fn){events.set(name,fn)}};
  const editor={},document={activeElement:editor,visibilityState:'visible',
    getElementById:id=>id==='editor'?editor:null,hasFocus:()=>true};
  const sandbox={window,document,Uint8Array,Uint32Array,DataView,Array,Number,
    performance:{now:()=>100},crypto:{subtle},
    createImageBitmap:async file=>({...(decoder(await file.buffer())),close(){}}),
    fetch:async()=>{throw Error('network disabled')},File:class {},Promise};
  sandbox.globalThis=sandbox;
  vm.runInNewContext(SOURCE,sandbox);
  function paste(data,{fileSize=data?.length,failRead=false}={}) {
    const file=data===null?null:{size:fileSize,type:'image/png',
      async arrayBuffer(){if(failRead)throw Error('read failure');return Uint8Array.from(data).buffer;},
      async buffer(){return data}};
    let prevented=false;
    events.get('paste')({isTrusted:true,preventDefault(){prevented=true},
      clipboardData:{types:['Files'],files:{length:1},items:[
        {kind:'file',type:'image/png',getAsFile(){return file}}
      ]}});
    assert.equal(prevented,true);
  }
  async function result() {
    for(let i=0;i<400;i++) {
      const r=window.ClipboardImageReceipt.getLastReport();
      if(r.phase==='complete')return r;
      await new Promise(done=>setTimeout(done,5));
    }
    throw Error('browser receipt timeout');
  }
  return {paste,result,window,sandbox,events};
}
async function main() {
  let count=0;
  const names=['chansrv_peer_1049471.png','mac_size_control_2286451.png','oversize_control.png'];
  const baseline=path.join(FILES,'standalone_exact_2401598.png');
  if(fs.existsSync(baseline)) names.push('standalone_exact_2401598.png');
  for(const name of names) {
    const bytes=fs.readFileSync(path.join(FILES,name));
    for(const subtle of [crypto.webcrypto.subtle,null]) {
      const h=testPage({subtle});h.paste(bytes);const r=await h.result();
      assert.equal(r.sha256,crypto.createHash('sha256').update(bytes).digest('hex'));
      assert.equal(r.digestMethod,subtle?'webcrypto':'js-fallback');
      assert.equal(r.digestError,null);assert.equal(r.decodeError,null);
      assert.equal(r.readBytes,bytes.length);
      const d=fullDecode(bytes);
      assert.equal(r.ihdrWidth,d.width);assert.equal(r.decodedWidth,d.width);
      assert.equal(r.ihdrHeight,d.height);assert.equal(r.decodedHeight,d.height);
      count++;
    }
  }
  const sample=fs.readFileSync(path.join(FILES,'mac_size_control_2286451.png'));
  {const h=testPage({subtle:{async digest(){throw Error('blocked')}}});
    h.paste(sample);const r=await h.result();
    assert.equal(r.digestMethod,'js-fallback');assert.equal(r.digestError,null);
    assert.match(r.digestWarning,/webcrypto/);count++;}
  {const h=testPage();h.paste(null);const r=await h.result();
    assert.equal(r.getAsFileNull,true);count++;}
  {const h=testPage();h.paste(sample,{failRead:true});const r=await h.result();
    assert.match(r.readError,/arrayBuffer/);count++;}
  {const h=testPage();h.paste(sample,{fileSize:sample.length+1});
    assert.equal((await h.result()).readError,'read-size-mismatch');count++;}
  {const h=testPage();h.paste(sample,{fileSize:64*1024*1024+1});
    assert.equal((await h.result()).readError,'file-size-limit');count++;}
  {const corrupted=Buffer.from(sample);let p=8;
    while(p<corrupted.length){const n=corrupted.readUInt32BE(p);
      if(corrupted.toString('ascii',p+4,p+8)==='IDAT'){
        corrupted.fill(0,p+8,p+72);
        corrupted.writeUInt32BE(crc32(corrupted.subarray(p+4,p+8+n)),p+8+n);break;}
      p+=12+n;
    }
    const h=testPage();h.paste(corrupted);
    assert.match((await h.result()).decodeError,/createImageBitmap/);count++;}
  {const h=testPage({decoder:bytes=>{const d=fullDecode(bytes);return{width:d.width+1,height:d.height}}});
    h.paste(sample);assert.equal((await h.result()).decodeError,'decoded-dimensions-mismatch');count++;}
  // A previous unresolved image must not overwrite the latest null-File result.
  {const h=testPage();let release;
    const file={size:sample.length,type:'image/png',
      arrayBuffer:()=>new Promise(done=>{release=done})};
    h.events.get('paste')({isTrusted:true,preventDefault(){},
      clipboardData:{types:['Files'],files:{length:1},items:[
        {kind:'file',type:'image/png',getAsFile(){return file}}]}});
    h.paste(null);release(Uint8Array.from(sample).buffer);
    await new Promise(done=>setImmediate(done));
    assert.equal(h.window.ClipboardImageReceipt.getLastReport().getAsFileNull,true);count++;}
  for(const n of [1,55,56,63,64,65,119,120,121,2003]){
    const h=testPage({subtle:null}),bytes=sample.subarray(0,n);
    h.paste(bytes);const r=await h.result();
    assert.equal(r.sha256,crypto.createHash('sha256').update(bytes).digest('hex'));count++;
  }
  console.log('PASS: '+count+' synthetic PNG readback/hash/decode scenarios');
}
main().catch(error=>{console.error(error);process.exitCode=1});
