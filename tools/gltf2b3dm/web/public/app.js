// app.js — gltf2b3dm 在线预览 / 转换前端
import * as THREE from 'three';
import { GLTFLoader } from './vendor/three/loaders/GLTFLoader.js';
import { DRACOLoader } from './vendor/three/loaders/DRACOLoader.js';
import { KTX2Loader } from './vendor/three/loaders/KTX2Loader.js';
import { OrbitControls } from './vendor/three/controls/OrbitControls.js';
import { MeshoptDecoder } from './vendor/three/libs/meshopt_decoder.module.js';
import GUI from './vendor/three/libs/lil-gui.module.min.js';

/* ---------- 转换参数（与 gltf2b3dm 命令行选项对应）---------- */
const params = {
  maxTris: 0,
  texSize: 256,
  detailTexSize: 1024,
  variant: '',
  materialMode: 'full',
  morphLockRatio: 0.002,
  morphLockMaxPct: 0.40,
  animError: 0.01,
  // 静态额外压缩 + 框选区域减面
  staticRatio: 1.0,
  regionEnable: false,
  regionMode: 'decimate',
  regionRatio: 0.3,
  // 框(模型归一化坐标)：中心偏移 [-1,1]、半尺寸 [0,1]，默认居中、覆盖中间一半
  regionCx: 0, regionCy: 0, regionCz: 0,
  regionSx: 0.5, regionSy: 0.5, regionSz: 0.5,
  autoConvert: true,
  wireframe: false,
  showNormals: false,
};

/* 由框选参数换算出 glTF 世界坐标 AABB 字符串 "x0,y0,z0,x1,y1,z1"(未启用返回 null) */
function computeRegionArg() {
  if (!params.regionEnable) return null;
  const c = viewerL.getModelCenter(), h = viewerL.getModelHalf();
  const cx = c.x + params.regionCx*h.x, cy = c.y + params.regionCy*h.y, cz = c.z + params.regionCz*h.z;
  const hx = params.regionSx*h.x, hy = params.regionSy*h.y, hz = params.regionSz*h.z;
  return [cx-hx, cy-hy, cz-hz, cx+hx, cy+hy, cz+hz].map(v => v.toFixed(4)).join(',');
}
/* 把当前框选参数同步到左侧线框显示 */
function refreshRegionBox() {
  if (typeof viewerL === 'undefined') return;
  viewerL.updateRegionBox(params.regionEnable,
    params.regionCx, params.regionCy, params.regionCz,
    params.regionSx, params.regionSy, params.regionSz);
}

let currentFile = null;     // 当前上传的原始文件（File）
let serverFileId = null;    // 服务端保存的文件 id
let lastB3dmName = null;    // 最近转换出的 b3dm 文件名（用于下载）
let syncTime = false;       // 左右动画时间同步(便于同帧对比)
let globalAnimTime = 0;     // 同步模式下的共享时间(秒)

/* ---------- 通用 three.js 查看器 ---------- */
function makeViewer(paneId) {
  const pane = document.getElementById(paneId);
  const renderer = new THREE.WebGLRenderer({ antialias: true });
  renderer.setPixelRatio(window.devicePixelRatio);
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  pane.appendChild(renderer.domElement);

  const scene = new THREE.Scene();
  scene.background = new THREE.Color(0x1a1d22);

  const camera = new THREE.PerspectiveCamera(45, 1, 0.01, 5000);
  camera.position.set(0, 0, 3);

  const controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = true;
  controls.dampingFactor = 0.08;

  // 光照：半球光 + 方向光（近似 demo 的固定光）
  scene.add(new THREE.HemisphereLight(0xffffff, 0x444455, 1.1));
  const dir = new THREE.DirectionalLight(0xffffff, 1.6);
  dir.position.set(2, 3, 4);
  scene.add(dir);
  const grid = new THREE.GridHelper(4, 16, 0x333740, 0x2a2e36);
  grid.position.y = -0.001;
  scene.add(grid);

  let root = null;       // 当前模型根
  let mixer = null;      // 动画混合器
  let clips = [];        // 动画 clip 列表
  let action = null;     // 当前播放的 action
  let skinnedCount = 0;  // SkinnedMesh 数量(诊断用)
  const clock = new THREE.Clock();
  // 框选区域：glTF 世界坐标下的模型中心/半尺寸(frameObject 时记录，未重定心前的世界 AABB)
  const modelCenter = new THREE.Vector3();
  const modelHalf = new THREE.Vector3(1, 1, 1);
  let regionHelper = null;  // 场景空间(已重定心)里的框线 Box3Helper

  function resize() {
    const w = pane.clientWidth, h = pane.clientHeight;
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
  }
  new ResizeObserver(resize).observe(pane);
  resize();

  function frameObject(obj) {
    const box = new THREE.Box3().setFromObject(obj);
    const size = box.getSize(new THREE.Vector3());
    const center = box.getCenter(new THREE.Vector3());
    /* 记录 glTF 世界坐标下的中心/半尺寸(重定心前)，供框选区域换算回 glTF 世界坐标 */
    modelCenter.copy(center);
    modelHalf.set(Math.max(size.x/2, 1e-6), Math.max(size.y/2, 1e-6), Math.max(size.z/2, 1e-6));
    const maxDim = Math.max(size.x, size.y, size.z) || 1;
    obj.position.sub(center);                 // 居中
    const dist = maxDim * 2.2;
    camera.position.set(0, maxDim * 0.3, dist);
    camera.near = maxDim / 100;
    camera.far = maxDim * 100;
    camera.updateProjectionMatrix();
    controls.target.set(0, 0, 0);
    controls.update();
    grid.scale.setScalar(maxDim / 2);
  }

  function clear() {
    if (root) { scene.remove(root); root.traverse(o => { if (o.geometry) o.geometry.dispose(); }); root = null; }
    if (mixer) { mixer.stopAllAction(); mixer = null; }
    clips = []; action = null;
  }

  function setModel(gltf) {
    clear();
    root = gltf.scene || gltf.scenes[0];
    scene.add(root);
    // SkinnedMesh / morph mesh：关闭视锥剔除(移动/动画/morph 后包围盒按 bind pose
    // 或全量顶点算会误剔除导致整网格消失)，并确保骨架矩阵就绪。
    skinnedCount = 0;
    root.traverse(o => {
      if (o.isSkinnedMesh) {
        skinnedCount++;
        o.frustumCulled = false;
        if (o.skeleton) o.skeleton.update();
      }
      // morph mesh：morphTargetInfluences 存在即有 morph，关闭剔除并强制重算包围体
      if (o.isMesh && o.geometry && o.geometry.morphAttributes &&
          o.geometry.morphAttributes.position) {
        o.frustumCulled = false;
        o.geometry.computeBoundingSphere();
        o.geometry.computeBoundingBox();
      }
    });
    frameObject(root);
    clips = gltf.animations || [];
    if (clips.length) {
      mixer = new THREE.AnimationMixer(root);
      action = mixer.clipAction(clips[0]);
      action.reset();
      action.play();
    }
    applyDisplayOpts();
    return computeStats(root, gltf);
  }

  function applyDisplayOpts() {
    if (!root) return;
    root.traverse(o => {
      if (o.isMesh) {
        o.material.wireframe = params.wireframe;
        o.material.needsUpdate = true;
      }
    });
  }

  function playClip(index) {
    if (!mixer || !clips.length) return;
    if (action) action.stop();
    const i = Math.max(0, Math.min(index, clips.length - 1));
    action = mixer.clipAction(clips[i]);
    action.reset();
    action.play();
  }
  function clipNames() { return clips.map((c, i) => c.name || ('clip' + i)); }

  // 设置动画到绝对时间(用于左右同步对比)。返回该 clip 时长。
  function setAnimTime(tSec) {
    if (!mixer || !action) return 0;
    const dur = action.getClip() ? action.getClip().duration : 0;
    const tt = dur > 0 ? (tSec % dur) : tSec;
    action.paused = true;
    action.time = tt;
    mixer.update(0);   // 立即应用到该时间
    return dur;
  }

  // 运行时动画诊断：是否有 mixer、当前 action 时间是否在推进
  function getAnimInfo() {
    if (!mixer || !action) return { has: false };
    return {
      has: true,
      running: action.isRunning(),
      time: action.time,
      clip: action.getClip() ? action.getClip().name : '?',
      skinned: skinnedCount,
    };
  }

  /* 更新/显示框选区域线框。入参为“模型归一化”坐标：
   *   c* ∈ 中心偏移(以模型半尺寸为单位，0=模型中心)；s* ∈ 半尺寸(以模型半尺寸为单位)。
   * 线框画在已重定心的场景空间(模型中心=场景原点)，故场景坐标 = 归一化 × modelHalf。 */
  function updateRegionBox(on, cx, cy, cz, sx, sy, sz) {
    if (regionHelper) { scene.remove(regionHelper); regionHelper = null; }
    if (!on) return;
    const ccx = cx*modelHalf.x, ccy = cy*modelHalf.y, ccz = cz*modelHalf.z;
    const hx = Math.max(sx*modelHalf.x, 1e-6), hy = Math.max(sy*modelHalf.y, 1e-6), hz = Math.max(sz*modelHalf.z, 1e-6);
    const b = new THREE.Box3(
      new THREE.Vector3(ccx-hx, ccy-hy, ccz-hz),
      new THREE.Vector3(ccx+hx, ccy+hy, ccz+hz));
    regionHelper = new THREE.Box3Helper(b, 0x00ff88);
    scene.add(regionHelper);
  }
  const getModelCenter = () => modelCenter.clone();
  const getModelHalf = () => modelHalf.clone();

  function animate() {
    requestAnimationFrame(animate);
    // 同步模式下时间由外部统一驱动(setAnimTime)，这里不再自增
    if (mixer && !syncTime) mixer.update(clock.getDelta());
    controls.update();
    renderer.render(scene, camera);
  }
  animate();

  return { setModel, clear, applyDisplayOpts, controls, camera, renderer, playClip, clipNames, getAnimInfo, setAnimTime,
           updateRegionBox, getModelCenter, getModelHalf };
}

function computeStats(root, gltf) {
  let tris = 0, verts = 0, meshes = 0, mats = new Set(), texs = new Set();
  root.traverse(o => {
    if (o.isMesh && o.geometry) {
      meshes++;
      const g = o.geometry;
      const idx = g.index ? g.index.count : (g.attributes.position ? g.attributes.position.count : 0);
      tris += idx / 3;
      verts += g.attributes.position ? g.attributes.position.count : 0;
      if (o.material) {
        mats.add(o.material.uuid);
        for (const k of ['map', 'normalMap', 'roughnessMap', 'metalnessMap', 'emissiveMap', 'aoMap']) {
          if (o.material[k]) texs.add(o.material[k].uuid);
        }
      }
    }
  });
  const anims = gltf && gltf.animations ? gltf.animations.length : 0;
  return { tris: Math.round(tris), verts, meshes, mats: mats.size, texs: texs.size, anims };
}

function fmtStats(s) {
  if (!s) return '';
  return `三角形: ${s.tris.toLocaleString()}\n顶点: ${s.verts.toLocaleString()}\n网格: ${s.meshes}  材质: ${s.mats}\n纹理: ${s.texs}  动画: ${s.anims}`;
}

/* ---------- GLTFLoader 配置（含 draco / meshopt / ktx2 解码）---------- */
function makeLoader(renderer) {
  const loader = new GLTFLoader();
  const draco = new DRACOLoader();
  draco.setDecoderPath('./vendor/three/libs/draco/gltf/');
  loader.setDRACOLoader(draco);
  loader.setMeshoptDecoder(MeshoptDecoder);
  try {
    const ktx2 = new KTX2Loader();
    ktx2.setTranscoderPath('./vendor/three/libs/basis/');
    if (renderer) ktx2.detectSupport(renderer);
    loader.setKTX2Loader(ktx2);
  } catch (e) { /* KTX2 可选 */ }
  return loader;
}

const viewerL = makeViewer('paneL');
const viewerR = makeViewer('paneR');
const loaderL = makeLoader(viewerL.renderer);
const loaderR = makeLoader(viewerR.renderer);

const $ = id => document.getElementById(id);
function log(msg) {
  const el = $('log'); el.style.display = 'block';
  el.textContent += msg + '\n'; el.scrollTop = el.scrollHeight;
}
function setStatus(s) { $('status').textContent = s; }

/* ---------- 加载原始 glTF 到左侧 ---------- */
function loadOriginal(arrayBuffer, name) {
  $('msgL').style.display = 'none';
  $('spinL').style.display = 'block';
  const blob = new Blob([arrayBuffer]);
  const url = URL.createObjectURL(blob);
  loaderL.load(url, gltf => {
    const s = viewerL.setModel(gltf);
    $('statsL').textContent = fmtStats(s);
    $('spinL').style.display = 'none';
    URL.revokeObjectURL(url);
    refreshAnimControl();
    refreshRegionBox();   // 模型尺寸变化后，按新模型重画框线
  }, undefined, err => {
    $('spinL').style.display = 'none';
    $('msgL').style.display = 'flex';
    $('msgL').textContent = '原始模型加载失败: ' + err;
    log('左侧加载失败: ' + err);
  });
}

/* ---------- 调服务端转换，再把反解 glTF 加载到右侧 ---------- */
async function convert() {
  if (!serverFileId) return;
  $('msgR').style.display = 'none';
  $('spinR').style.display = 'block';
  setStatus('转换中…');
  try {
    const resp = await fetch('/api/convert', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        fileId: serverFileId,
        origName: currentFile ? currentFile.name : '',
        maxTris: params.maxTris,
        texSize: params.texSize,
        detailTexSize: params.detailTexSize,
        variant: params.variant,
        materialMode: params.materialMode,
        morphLockRatio: params.morphLockRatio,
        morphLockMaxPct: params.morphLockMaxPct,
        animError: params.animError,
        staticRatio: params.staticRatio,
        region: computeRegionArg(),               // null=未启用框选
        regionMode: params.regionMode,
        regionRatio: params.regionRatio,
      }),
    });
    const data = await resp.json();
    if (!data.ok) throw new Error(data.error || '转换失败');
    log(data.toolOutput || '');
    lastB3dmName = data.b3dmName;
    $('download').disabled = false;

    // 反解出的 glTF（服务端转成 .gltf + .bin）用 URL 加载
    const url = '/api/preview/' + data.previewId + '/scene.gltf';
    loaderR.load(url, gltf => {
      const s = viewerR.setModel(gltf);
      // 右侧统计加上工具报告的三角形/纹理数
      $('statsR').textContent = fmtStats(s) +
        (data.tris != null ? `\n— 工具报告 —\n三角形: ${Number(data.tris).toLocaleString()}  纹理: ${data.texs}` : '');
      $('spinR').style.display = 'none';
      setStatus(`转换完成: ${data.b3dmName}  (${Number(data.tris).toLocaleString()} 三角形, ${data.sizeKB} KB)`);
      refreshAnimControl();
    }, undefined, err => {
      $('spinR').style.display = 'none';
      $('msgR').style.display = 'flex';
      $('msgR').textContent = '反解预览加载失败';
      log('右侧加载失败: ' + err);
    });
  } catch (e) {
    $('spinR').style.display = 'none';
    $('msgR').style.display = 'flex';
    $('msgR').textContent = '转换失败: ' + e.message;
    setStatus('转换失败');
    log('转换错误: ' + e.message);
  }
}

/* ---------- 上传文件到服务端 ---------- */
async function uploadAndLoad(file) {
  currentFile = file;
  setStatus('上传 ' + file.name + ' …');
  const buf = await file.arrayBuffer();
  loadOriginal(buf.slice(0), file.name);

  const fd = new FormData();
  fd.append('file', new Blob([buf]), file.name);
  const resp = await fetch('/api/upload', { method: 'POST', body: fd });
  const data = await resp.json();
  if (!data.ok) { setStatus('上传失败'); log('上传失败: ' + data.error); return; }
  serverFileId = data.fileId;
  setStatus('已加载 ' + file.name + (data.compression ? '（压缩: ' + data.compression + '）' : ''));
  if (params.autoConvert) convert();
}

/* ---------- 文件选择 / 拖放 ---------- */
$('file').addEventListener('change', e => { if (e.target.files[0]) uploadAndLoad(e.target.files[0]); });
document.body.addEventListener('dragover', e => { e.preventDefault(); });
document.body.addEventListener('drop', e => {
  e.preventDefault();
  const f = e.dataTransfer.files[0];
  if (f && /\.(gltf|glb)$/i.test(f.name)) uploadAndLoad(f);
});
$('reconvert').addEventListener('click', convert);
$('download').addEventListener('click', () => {
  if (lastB3dmName) window.location = '/api/download/' + encodeURIComponent(lastB3dmName);
});

/* ---------- 参数面板（lil-gui）---------- */
const gui = new GUI({ container: $('panel'), title: '转换参数' });
let convertTimer = null;
function scheduleConvert() {
  if (!params.autoConvert || !serverFileId) return;
  clearTimeout(convertTimer);
  convertTimer = setTimeout(convert, 400);
}
const fConv = gui.addFolder('gltf2b3dm 选项');
fConv.add(params, 'maxTris', 0, 200000, 1).name('减面预算 (0=不减)').onFinishChange(scheduleConvert);
fConv.add(params, 'texSize', [64, 128, 256, 512, 1024]).name('纹理上限').onFinishChange(scheduleConvert);
fConv.add(params, 'detailTexSize', [256, 512, 1024, 2048]).name('高细节纹理上限').onFinishChange(scheduleConvert);
fConv.add(params, 'materialMode', ['full', 'baked-vertex', 'solid', 'none']).name('材质模式').onFinishChange(scheduleConvert);
fConv.add(params, 'variant').name('材质变体子串').onFinishChange(scheduleConvert);

const fMorph = gui.addFolder('动画网格减面保护');
fMorph.add(params, 'morphLockRatio', 0.0005, 0.08, 0.0005).name('morph 锁定阈值 (小=锁更多)').onFinishChange(scheduleConvert);
fMorph.add(params, 'morphLockMaxPct', 0.1, 0.95, 0.05).name('锁定占比上限').onFinishChange(scheduleConvert);
fMorph.add(params, 'animError', 0.001, 0.1, 0.001).name('动画减面误差').onFinishChange(scheduleConvert);
fMorph.open();

/* 静态额外压缩 + 框选区域减面 */
const fRegion = gui.addFolder('静态 / 框选区域减面');
fRegion.add(params, 'staticRatio', 0.05, 1.0, 0.05).name('非动画件额外压缩(1=不压)').onFinishChange(scheduleConvert);
fRegion.add(params, 'regionEnable').name('启用框选区域').onChange(() => { refreshRegionBox(); scheduleConvert(); });
fRegion.add(params, 'regionMode', ['decimate', 'protect']).name('框内: 减面/保面').onFinishChange(scheduleConvert);
fRegion.add(params, 'regionRatio', 0.02, 1.0, 0.02).name('框内减面系数(decimate)').onFinishChange(scheduleConvert);
const fBox = fRegion.addFolder('框(模型归一化: 中心/半尺寸)');
const boxChange = () => { refreshRegionBox(); };
fBox.add(params, 'regionCx', -1, 1, 0.02).name('中心X').onChange(boxChange).onFinishChange(scheduleConvert);
fBox.add(params, 'regionCy', -1, 1, 0.02).name('中心Y').onChange(boxChange).onFinishChange(scheduleConvert);
fBox.add(params, 'regionCz', -1, 1, 0.02).name('中心Z').onChange(boxChange).onFinishChange(scheduleConvert);
fBox.add(params, 'regionSx', 0.02, 1, 0.02).name('半尺寸X').onChange(boxChange).onFinishChange(scheduleConvert);
fBox.add(params, 'regionSy', 0.02, 1, 0.02).name('半尺寸Y').onChange(boxChange).onFinishChange(scheduleConvert);
fBox.add(params, 'regionSz', 0.02, 1, 0.02).name('半尺寸Z').onChange(boxChange).onFinishChange(scheduleConvert);
fBox.open();
fRegion.open();

fConv.add(params, 'autoConvert').name('参数改动自动转换');
fConv.open();

const fView = gui.addFolder('显示');
fView.add(params, 'wireframe').name('线框').onChange(() => { viewerL.applyDisplayOpts(); viewerR.applyDisplayOpts(); });
fView.add({ sync: () => {
  // 把左侧相机同步到右侧
  viewerR.camera.position.copy(viewerL.camera.position);
  viewerR.controls.target.copy(viewerL.controls.target);
  viewerR.controls.update();
} }, 'sync').name('右侧视角=左侧');
fView.open();

/* ---------- 动画选择（左右同步播放同一 clip）---------- */
const fAnim = gui.addFolder('动画');
const animState = { clip: '(无)' };
let animCtrl = null;
function refreshAnimControl() {
  // 以左侧(原始)的 clip 列表为准；右侧若有同名/同序也一起切
  const namesL = viewerL.clipNames();
  const names = namesL.length ? namesL : viewerR.clipNames();
  if (animCtrl) { animCtrl.destroy(); animCtrl = null; }
  if (!names.length) {
    animState.clip = '(无)';
    return;
  }
  animState.clip = names[0];
  animCtrl = fAnim.add(animState, 'clip', names).name('播放').onChange(name => {
    const i = names.indexOf(name);
    viewerL.playClip(i);
    viewerR.playClip(i);   // 右侧 clip 顺序与左侧一致(b3dm 保留原序)
    globalAnimTime = 0;
  });
  fAnim.open();
}
refreshAnimControl();

/* ---------- 左右动画时间同步(同帧对比) ---------- */
const syncState = { sync: false, time: 0, playing: true };
const fSync = gui.addFolder('对比');
fSync.add(syncState, 'sync').name('同步左右时间').onChange(v => {
  syncTime = v;
  if (!v) { // 退出同步：恢复各自播放
    viewerL.playClip(0); viewerR.playClip(0);
  }
});
fSync.add(syncState, 'playing').name('同步-播放/暂停');
const timeCtrl = fSync.add(syncState, 'time', 0, 1, 0.001).name('同步-时间轴').onChange(v => {
  if (syncTime) { globalAnimTime = v; }
});
fSync.open();

// 同步驱动：统一推进 globalAnimTime，写入左右(同一绝对时间→同一帧)
let lastTs = performance.now();
function syncDriver() {
  requestAnimationFrame(syncDriver);
  const now = performance.now();
  const dt = (now - lastTs) / 1000; lastTs = now;
  if (!syncTime) return;
  if (syncState.playing) globalAnimTime += dt;
  const durL = viewerL.setAnimTime(globalAnimTime);
  const durR = viewerR.setAnimTime(globalAnimTime);
  // 用左侧时长归一化时间轴滑块显示
  const dur = durL || durR || 1;
  syncState.time = globalAnimTime % dur;
  if (timeCtrl && timeCtrl.max !== dur) { timeCtrl.max(dur); }
  timeCtrl.updateDisplay();
}
syncDriver();

/* ---------- 运行时动画诊断（每 0.5s 刷新到角标）---------- */
function animDiag(info) {
  if (!info || !info.has) return 'animation: 无 mixer/clip';
  return `animation: ${info.running ? '播放中' : '停止'}  t=${info.time.toFixed(2)}s` +
         `  clip=${info.clip}  skinned=${info.skinned}`;
}
setInterval(() => {
  const dl = document.getElementById('diagL');
  const dr = document.getElementById('diagR');
  if (dl) dl.textContent = animDiag(viewerL.getAnimInfo());
  if (dr) dr.textContent = animDiag(viewerR.getAnimInfo());
}, 500);

/* ---------- 把运行时错误暴露到屏幕日志(便于无 console 时定位) ---------- */
window.addEventListener('error', e => {
  try { log('[JS错误] ' + (e.message || e.error || e)); } catch (_) {}
});
window.addEventListener('unhandledrejection', e => {
  try { log('[Promise错误] ' + (e.reason && e.reason.message ? e.reason.message : e.reason)); } catch (_) {}
});
