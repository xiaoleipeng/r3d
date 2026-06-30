// server.js — gltf2b3dm 在线工具本地服务（仅用 Node 内置模块，无需 npm install）
//
// 端点:
//   POST /api/upload            上传 glTF/glb，返回 fileId + 压缩信息
//   POST /api/convert           调 gltf2b3dm 转换，再调 b3dm2gltf 反解供预览
//   GET  /api/preview/:id/*     预览用的反解 gltf/bin/png 静态文件
//   GET  /api/download/:name    下载生成的 b3dm
//   GET  /*                     public/ 静态文件
//
// 用法: node server.js [--port 8787] [--tool <gltf2b3dm 路径>] [--verify <b3dm2gltf 路径>]

const http = require('http');
const fs = require('fs');
const path = require('path');
const os = require('os');
const { execFile } = require('child_process');
const crypto = require('crypto');

/* ---------- 参数解析 ---------- */
const argv = process.argv.slice(2);
function getArg(name, def) { const i = argv.indexOf(name); return i >= 0 && argv[i + 1] ? argv[i + 1] : def; }

const PORT = parseInt(getArg('--port', '8787'), 10);
const ROOT = __dirname;
const PUBLIC = path.join(ROOT, 'public');
const WORK = path.join(os.tmpdir(), 'gltf2b3dm_web');
const UP = path.join(WORK, 'uploads');
const OUT = path.join(WORK, 'out');
const PREV = path.join(WORK, 'preview');
for (const d of [WORK, UP, OUT, PREV]) fs.mkdirSync(d, { recursive: true });

// 自动定位工具二进制（优先命令行指定，其次常见构建目录）
function findTool(name, override) {
  if (override && fs.existsSync(override)) return override;
  const cands = [
    path.join('/tmp/r3d_build2', name),
    path.join(ROOT, '../../../build', name),
    path.join(ROOT, '../../../../build', name),
  ];
  for (const c of cands) if (fs.existsSync(c)) return c;
  return null;
}
const TOOL = findTool('gltf2b3dm', getArg('--tool'));
const VERIFY = findTool('b3dm2gltf', getArg('--verify'));

if (!TOOL) {
  console.error('找不到 gltf2b3dm，请用 --tool <路径> 指定，或先构建到 /tmp/r3d_build2');
  process.exit(1);
}
console.log('gltf2b3dm:', TOOL);
console.log('b3dm2gltf:', VERIFY || '(未找到，b3dm 预览将不可用)');

/* ---------- 工具函数 ---------- */
const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.json': 'application/json', '.gltf': 'model/gltf+json', '.bin': 'application/octet-stream',
  '.glb': 'model/gltf-binary', '.b3dm': 'application/octet-stream',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.wasm': 'application/wasm',
  '.svg': 'image/svg+xml',
};
function send(res, code, body, headers = {}) {
  res.writeHead(code, headers); res.end(body);
}
function sendJson(res, code, obj) { send(res, code, JSON.stringify(obj), { 'Content-Type': 'application/json' }); }

function serveStatic(res, filePath) {
  fs.readFile(filePath, (err, data) => {
    if (err) return send(res, 404, 'Not found');
    const ext = path.extname(filePath).toLowerCase();
    send(res, 200, data, { 'Content-Type': MIME[ext] || 'application/octet-stream' });
  });
}

// 安全路径拼接，防目录穿越
function safeJoin(base, target) {
  const p = path.normalize(path.join(base, target));
  if (!p.startsWith(base)) return null;
  return p;
}

// 检测 glb 压缩扩展（仅用于提示）
function detectCompression(buf) {
  try {
    if (buf.length < 12 || buf.readUInt32LE(0) !== 0x46546c67) return null; // 'glTF'
    const jsonLen = buf.readUInt32LE(12);
    const json = JSON.parse(buf.slice(20, 20 + jsonLen).toString('utf8'));
    const used = json.extensionsUsed || [];
    const tags = [];
    if (used.includes('EXT_meshopt_compression')) tags.push('meshopt');
    if (used.includes('KHR_draco_mesh_compression')) tags.push('draco');
    return tags.length ? tags.join('+') : null;
  } catch { return null; }
}

/* ---------- multipart/form-data 解析（单文件，内存）---------- */
function parseMultipart(req, cb) {
  const ct = req.headers['content-type'] || '';
  const m = ct.match(/boundary=(.+)$/);
  if (!m) return cb(new Error('no boundary'));
  const boundary = Buffer.from('--' + m[1]);
  const chunks = [];
  req.on('data', c => chunks.push(c));
  req.on('end', () => {
    const buf = Buffer.concat(chunks);
    // 找第一个 part 的 header/body 分隔
    let start = buf.indexOf(boundary) + boundary.length + 2; // \r\n
    const headerEnd = buf.indexOf('\r\n\r\n', start);
    if (headerEnd < 0) return cb(new Error('bad multipart'));
    const header = buf.slice(start, headerEnd).toString();
    const nameM = header.match(/filename="([^"]*)"/);
    const filename = nameM ? nameM[1] : 'upload.bin';
    const bodyStart = headerEnd + 4;
    const bodyEnd = buf.indexOf('\r\n' + boundary, bodyStart);
    const body = buf.slice(bodyStart, bodyEnd < 0 ? buf.length : bodyEnd);
    cb(null, { filename, data: body });
  });
  req.on('error', cb);
}

function readBody(req, cb) {
  const chunks = [];
  req.on('data', c => chunks.push(c));
  req.on('end', () => cb(null, Buffer.concat(chunks)));
  req.on('error', cb);
}

/* ---------- 路由 ---------- */
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const pathname = decodeURIComponent(url.pathname);

  // --- 上传 ---
  if (req.method === 'POST' && pathname === '/api/upload') {
    return parseMultipart(req, (err, part) => {
      if (err) return sendJson(res, 400, { ok: false, error: err.message });
      const ext = /\.glb$/i.test(part.filename) ? '.glb' : '.gltf';
      const fileId = crypto.randomBytes(8).toString('hex');
      const dest = path.join(UP, fileId + ext);
      fs.writeFileSync(dest, part.data);
      const compression = detectCompression(part.data);
      sendJson(res, 200, { ok: true, fileId: fileId + ext, name: part.filename, compression });
    });
  }

  // --- 转换 ---
  if (req.method === 'POST' && pathname === '/api/convert') {
    return readBody(req, (err, body) => {
      if (err) return sendJson(res, 400, { ok: false, error: err.message });
      let opt;
      try { opt = JSON.parse(body.toString()); } catch { return sendJson(res, 400, { ok: false, error: 'bad json' }); }
      const src = safeJoin(UP, opt.fileId || '');
      if (!src || !fs.existsSync(src)) return sendJson(res, 400, { ok: false, error: 'file not found' });

      const base = path.basename(opt.origName || path.basename(src))
                     .replace(/\.(gltf|glb)$/i, '')
                     .replace(/[^\w.\-]/g, '_');   // 清掉路径/特殊字符，安全文件名
      const tmpName = '_tmp_' + crypto.randomBytes(4).toString('hex') + '.b3dm';
      const b3dmPath = path.join(OUT, tmpName);   // 先转到临时名，拿到面数后重命名

      const args = [src, b3dmPath];
      if (opt.maxTris) args.push('--max-tris', String(opt.maxTris));
      if (opt.texSize) args.push('--tex-size', String(opt.texSize));
      if (opt.detailTexSize) args.push('--detail-tex-size', String(opt.detailTexSize));
      if (opt.variant) args.push('--variant', String(opt.variant));
      if (opt.materialMode && opt.materialMode !== 'full') args.push('--material-mode', String(opt.materialMode));
      if (opt.morphLockRatio != null) args.push('--morph-lock-ratio', String(opt.morphLockRatio));
      if (opt.morphLockMaxPct != null) args.push('--morph-lock-max-pct', String(opt.morphLockMaxPct));
      if (opt.animError != null) args.push('--anim-error', String(opt.animError));

      execFile(TOOL, args, { timeout: 120000, maxBuffer: 16 * 1024 * 1024 }, (e, stdout, stderr) => {
        const toolOutput = (stderr || '') + (stdout || '');
        if (e) return sendJson(res, 200, { ok: false, error: '转换失败: ' + (e.message || ''), toolOutput });

        // 解析工具 stdout: "(prims=.. tris=.. texs=.. clips=..)"
        let tris = null, texs = null;
        const mm = toolOutput.match(/tris=(\d+).*?texs=(\d+)/s);
        if (mm) { tris = parseInt(mm[1], 10); texs = parseInt(mm[2], 10); }

        // 按"原始文件名_三角面数_材质模式.b3dm"命名
        // (面数转换后才知道，故先转临时名再重命名；full 模式也显式标注便于区分)
        const matMode = (opt.materialMode && /^(full|baked-vertex|solid|none)$/.test(opt.materialMode))
                        ? opt.materialMode : 'full';
        const b3dmName = base
                       + (tris != null ? '_' + tris : '')
                       + '_' + matMode
                       + '.b3dm';
        const finalPath = path.join(OUT, b3dmName);
        try { fs.renameSync(b3dmPath, finalPath); }
        catch (re) { return sendJson(res, 200, { ok: false, error: '重命名失败: ' + re.message, toolOutput }); }

        const sizeKB = Math.round(fs.statSync(finalPath).size / 1024);

        // 反解 b3dm → gltf 供预览
        if (!VERIFY) return sendJson(res, 200, { ok: true, b3dmName, tris, texs, sizeKB, toolOutput, previewId: null });
        const previewId = crypto.randomBytes(8).toString('hex');
        const previewDir = path.join(PREV, previewId);
        fs.mkdirSync(previewDir, { recursive: true });
        const gltfOut = path.join(previewDir, 'scene.gltf');
        execFile(VERIFY, [finalPath, gltfOut], { timeout: 120000, maxBuffer: 16 * 1024 * 1024 }, (e2, so2, se2) => {
          if (e2) return sendJson(res, 200, { ok: true, b3dmName, tris, texs, sizeKB, toolOutput, previewId: null, previewError: (se2 || e2.message) });
          sendJson(res, 200, { ok: true, b3dmName, tris, texs, sizeKB, toolOutput, previewId });
        });
      });
    });
  }

  // --- 预览静态（反解产物）---
  if (req.method === 'GET' && pathname.startsWith('/api/preview/')) {
    const rel = pathname.slice('/api/preview/'.length); // <id>/scene.gltf
    const fp = safeJoin(PREV, rel);
    if (!fp) return send(res, 403, 'forbidden');
    return serveStatic(res, fp);
  }

  // --- 下载 b3dm ---
  if (req.method === 'GET' && pathname.startsWith('/api/download/')) {
    const name = pathname.slice('/api/download/'.length);
    const fp = safeJoin(OUT, name);
    if (!fp || !fs.existsSync(fp)) return send(res, 404, 'not found');
    return fs.readFile(fp, (e, d) => {
      if (e) return send(res, 404, 'not found');
      send(res, 200, d, { 'Content-Type': 'application/octet-stream', 'Content-Disposition': `attachment; filename="${path.basename(name)}"` });
    });
  }

  // --- 静态文件 ---
  let rel = pathname === '/' ? '/index.html' : pathname;
  const fp = safeJoin(PUBLIC, rel);
  if (!fp) return send(res, 403, 'forbidden');
  serveStatic(res, fp);
});

server.listen(PORT, () => {
  console.log(`\n  gltf2b3dm 在线工具已启动:  http://localhost:${PORT}\n`);
});
