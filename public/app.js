/* 藤のnetdisk 前端：登录 / 文档编辑 / AI 助手 / 分片断点续传 / 在线预览 */
(function () {
  "use strict";

  const CHUNK_SIZE = 8 * 1024 * 1024;      // 每片 8MB
  const MAX_RUNNING = 3;                    // 并发上传数
  const MAX_TEXT_PREVIEW = 512 * 1024;
  const MAX_EDIT_TEXT = 2 * 1024 * 1024;    // 编辑器允许的最大文本文件
  const AI_MAX_CONTEXT = 80000;             // 传给 AI 的文档上下文上限（字符）

  const $ = (id) => document.getElementById(id);
  const state = {
    user: "",
    cwd: [],            // 当前目录各层名称
    entries: [],
    view: localStorage.getItem("aurora-view") === "grid" ? "grid" : "list",
    uploads: new Map(), // id -> upload item
    queue: [],
    running: 0,
    editor: null,       // { name, segments, text, dirty, mode }
    aiDoc: null,        // AI 面板打开时绑定的文档
    aiResult: "",
    oauth: { github: false, apple: false }
  };

  /* ---------------- 基础工具 ---------------- */
  function escapeHtml(value) {
    return String(value).replace(/[&<>"']/g, (ch) => ({
      "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
    }[ch]));
  }

  function relPath(segments) {
    return segments.map(encodeURIComponent).join("/");
  }

  function formatSize(bytes) {
    if (bytes == null) return "-";
    if (bytes === 0) return "0 B";
    const units = ["B", "KB", "MB", "GB", "TB", "PB"];
    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
      value /= 1024;
      unit += 1;
    }
    return (value >= 100 ? Math.round(value) : value.toFixed(value >= 10 ? 1 : 2)) + " " + units[unit];
  }

  function formatTime(iso) {
    if (!iso) return "-";
    const date = new Date(iso);
    if (Number.isNaN(date.getTime())) return "-";
    const now = new Date();
    const sameYear = date.getFullYear() === now.getFullYear();
    const pad = (n) => String(n).padStart(2, "0");
    const time = pad(date.getHours()) + ":" + pad(date.getMinutes());
    const day = pad(date.getMonth() + 1) + "-" + pad(date.getDate());
    return sameYear ? day + " " + time : date.getFullYear() + "-" + day;
  }

  function toast(message, type) {
    const box = document.createElement("div");
    box.className = "toast" + (type ? " " + type : "");
    box.textContent = message;
    $("toasts").appendChild(box);
    setTimeout(() => {
      box.style.opacity = "0";
      box.style.transition = "opacity .3s";
      setTimeout(() => box.remove(), 320);
    }, 3400);
  }

  class ApiError extends Error {
    constructor(status, message) {
      super(message || ("请求失败 (" + status + ")"));
      this.status = status;
    }
  }

  async function api(path, options) {
    const response = await fetch(path, Object.assign({ credentials: "same-origin" }, options));
    let payload = null;
    const type = response.headers.get("content-type") || "";
    if (type.includes("json")) {
      payload = await response.json().catch(() => null);
    }
    if (!response.ok) {
      if (response.status === 401) {
        showLogin();
      }
      throw new ApiError(response.status, payload && payload.error
        ? payload.error : "HTTP " + response.status);
    }
    return payload;
  }

  /* ---------------- 图标 ---------------- */
  const ICONS = {
    folder: '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
    file: '<path d="M13 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z"/><path d="M13 3v6h6"/>',
    video: '<rect x="2.5" y="6" width="13" height="12" rx="2"/><path d="m15.5 10 6-3v10l-6-3z"/>',
    audio: '<path d="M9 18V6l10-2v11"/><circle cx="6.5" cy="18" r="2.5"/><circle cx="16.5" cy="15" r="2.5"/>',
    image: '<rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="9" cy="10" r="2"/><path d="m3 18 6-6 4 4 3-3 5 5"/>',
    archive: '<path d="M21 8 12 3 3 8v8l9 5 9-5z"/><path d="M3 8l9 5 9-5"/><path d="M12 13v8"/>',
    download: '<path d="M12 4v12"/><path d="m7 11 5 5 5-5"/><path d="M5 20h14"/>',
    play: '<circle cx="12" cy="12" r="9"/><path d="m10 9 5 3-5 3z"/>',
    edit: '<path d="M4 20h4L19 9l-4-4L4 16z"/><path d="m13.5 6.5 4 4"/>',
    write: '<path d="M13 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z"/><path d="M13 3v6h6"/><path d="M9 15h6M9 11h2M9 19h4"/>',
    trash: '<path d="M4 7h16M9 7V5h6v2M6 7l1 13h10l1-13"/>',
    refresh: '<path d="M21 12a9 9 0 1 1-2.64-6.36"/><path d="M21 3v6h-6"/>'
  };

  function svgIcon(name, size) {
    return '<svg viewBox="0 0 24 24" width="' + (size || 17) + '" height="' + (size || 17) +
      '" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">' +
      (ICONS[name] || ICONS.file) + "</svg>";
  }

  const TEXT_EXT = new Set(["txt", "md", "json", "js", "ts", "css", "html", "htm", "xml",
    "csv", "log", "ini", "conf", "yml", "yaml", "toml", "sh", "py", "c", "cpp", "h",
    "java", "go", "rs", "sql", "properties"]);

  function textFileExt(name) {
    return (name.split(".").pop() || "").toLowerCase();
  }

  function isTextFile(entry) {
    return entry.type !== "directory" && TEXT_EXT.has(textFileExt(entry.name));
  }

  function entryKind(entry) {
    if (entry.type === "directory") return "folder";
    const ext = (entry.name.split(".").pop() || "").toLowerCase();
    if (["mp4", "webm", "ogv", "mov", "m4v"].includes(ext)) return "video";
    if (["mp3", "wav", "ogg", "oga", "m4a", "aac", "flac", "opus"].includes(ext)) return "audio";
    if (["png", "jpg", "jpeg", "gif", "webp", "svg", "bmp", "ico", "avif"].includes(ext)) return "image";
    if (["zip", "tar", "gz", "7z", "rar", "bz2", "xz"].includes(ext)) return "archive";
    if (ext === "pdf") return "pdf";
    if (TEXT_EXT.has(ext)) return "text";
    return "file";
  }

  /* ---------------- 视图切换 ---------------- */
  function showLogin() {
    $("view-app").classList.add("hidden");
    $("view-login").classList.remove("hidden");
    $("upload-panel").classList.add("hidden");
    $("editor-modal").classList.add("hidden");
    $("ai-modal").classList.add("hidden");
    $("ai-config-modal").classList.add("hidden");
    state.editor = null;
    state.aiDoc = null;
    state.uploads.clear();
    renderUploadPanel();
    refreshOAuth();
  }

  function showApp(username) {
    state.user = username || "";
    $("view-login").classList.add("hidden");
    $("view-app").classList.remove("hidden");
    const avatar = $("user-avatar");
    avatar.textContent = (username || "A").slice(0, 1).toUpperCase();
    $("user-name").textContent = username || "";
    loadDir([], true);
  }

  /* ---------------- 文件浏览 ---------------- */
  async function loadDir(segments, force) {
    state.cwd = segments;
    $("file-area").innerHTML = "";
    if (!force) {
      renderBreadcrumbs();
    }
    try {
      const query = segments.length ? "?path=" + relPath(segments) : "";
      const data = await api("/api/drive/list" + query);
      state.entries = data.entries || [];
      renderEntries();
      const storage = $("storage-info");
      if (typeof data.total_bytes === "number" && typeof data.free_bytes === "number") {
        storage.textContent = "磁盘总量 " + formatSize(data.total_bytes) +
          " · 可用 " + formatSize(data.free_bytes);
      } else {
        storage.textContent = "";
      }
    } catch (error) {
      toast(error.message, "error");
    }
  }

  function renderBreadcrumbs() {
    const nav = $("breadcrumbs");
    nav.innerHTML = "";
    const root = document.createElement("button");
    root.className = "crumb" + (state.cwd.length === 0 ? " current" : "");
    root.textContent = "我的云盘";
    root.addEventListener("click", () => loadDir([], false));
    nav.appendChild(root);
    state.cwd.forEach((segment, index) => {
      const sep = document.createElement("span");
      sep.className = "crumb-sep";
      sep.textContent = "/";
      nav.appendChild(sep);
      const crumb = document.createElement("button");
      crumb.className = "crumb" + (index === state.cwd.length - 1 ? " current" : "");
      crumb.textContent = segment;
      crumb.title = segment;
      if (index < state.cwd.length - 1) {
        crumb.addEventListener("click", () => loadDir(state.cwd.slice(0, index + 1), false));
      }
      nav.appendChild(crumb);
    });
  }

  function renderEntries() {
    const area = $("file-area");
    const empty = $("empty-state");
    renderBreadcrumbs();
    empty.classList.toggle("hidden", state.entries.length > 0);
    area.innerHTML = "";
    if (state.entries.length === 0) return;

    const isList = state.view === "list";
    if (!isList) {
      area.classList.add("view-grid");
      area.classList.remove("view-list");
    } else {
      area.classList.add("view-list");
      area.classList.remove("view-grid");
    }

    if (isList) {
      const head = document.createElement("div");
      head.className = "file-head";
      head.innerHTML = '<span>名称</span><span>大小</span><span>修改时间</span><span></span>';
      area.appendChild(head);
    }

    state.entries.forEach((entry) => {
      const isDir = entry.type === "directory";
      const kind = isDir ? "folder" : entryKind(entry);
      const row = document.createElement(isList ? "div" : "a");
      row.className = "file-row" + (kind === "folder" ? " is-dir" : "");
      row.tabIndex = 0;
      if (isDir) {
        row.addEventListener("click", () => loadDir(state.cwd.concat(entry.name), false));
      }
      row.innerHTML =
        '<div class="col col-main"><span class="file-ico ' + kind + '">' +
        svgIcon(kind === "folder" ? "folder" : (kind === "video" || kind === "audio"
          ? "video" : (kind === "image" ? "image" : (kind === "archive" ? "archive" : "file"))), 18) +
        '</span><span class="file-name">' + escapeHtml(entry.name) + "</span></div>" +
        '<div class="col col-size ' + (isDir ? "col-empty" : "col-dim") + '">' +
        (isDir ? "-" : formatSize(entry.size)) + "</div>" +
        '<div class="col col-time col-dim">' + formatTime(entry.mtime) + "</div>" +
        '<div class="col row-actions">' +
        (isDir ? "" :
          '<button class="icon-btn" data-act="preview" title="在线预览" aria-label="预览">' +
          svgIcon("play", 15) + "</button>" +
          '<button class="icon-btn" data-act="download" title="下载" aria-label="下载">' +
          svgIcon("download", 15) + "</button>" +
          (isTextFile(entry) ?
            '<button class="icon-btn" data-act="edit" title="用编辑器打开" aria-label="编辑">' +
            svgIcon("write", 15) + "</button>" : "")) +
        '<button class="icon-btn" data-act="rename" title="重命名" aria-label="重命名">' +
        svgIcon("edit", 15) + "</button>" +
        '<button class="icon-btn" data-act="remove" title="删除" aria-label="删除">' +
        svgIcon("trash", 15) + "</button></div>";

      const targetPath = state.cwd.concat(entry.name);
      row.querySelectorAll("[data-act]").forEach((button) => {
        button.addEventListener("click", (event) => {
          event.stopPropagation();
          const action = button.dataset.act;
          if (action === "download") downloadEntry(targetPath);
          else if (action === "preview") previewEntry(entry, targetPath);
          else if (action === "edit") {
            if (entry.size > MAX_EDIT_TEXT) {
              toast("文件超过 " + formatSize(MAX_EDIT_TEXT) + "，请下载后用本地编辑器打开", "error");
              return;
            }
            openEditor(entry, targetPath);
          }
          else if (action === "rename") renameEntry(entry, targetPath);
          else if (action === "remove") removeEntry(entry, targetPath);
        });
      });
      area.appendChild(row);
    });
  }

  function entryUrl(kind, segments) {
    return "/api/drive/" + kind + "?path=" + relPath(segments);
  }

  function downloadEntry(segments) {
    const anchor = document.createElement("a");
    anchor.href = entryUrl("download", segments);
    anchor.style.display = "none";
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
  }

  async function removeEntry(entry, segments) {
    const label = entry.type === "directory" ? "文件夹及其中所有内容" : "文件";
    if (!window.confirm("确定删除" + label + "「" + entry.name + "」吗？此操作不可恢复。")) return;
    try {
      await api("/api/drive/remove?path=" + relPath(segments), { method: "DELETE" });
      toast("已删除「" + entry.name + "」", "ok");
      loadDir(state.cwd, true);
    } catch (error) {
      toast(error.message, "error");
    }
  }

  async function renameEntry(entry, segments) {
    const current = entry.name;
    const next = window.prompt("输入新名称：", current);
    if (next == null || next.trim() === "" || next === current) return;
    const name = next.trim();
    if (name.includes("/") || name.includes("\\")) {
      toast("名称不能包含 / 或 \\", "error");
      return;
    }
    try {
      await api("/api/drive/rename?from=" + relPath(segments) +
        "&to=" + relPath(state.cwd.concat(name)), { method: "POST", body: "" });
      toast("已重命名", "ok");
      loadDir(state.cwd, true);
    } catch (error) {
      toast(error.message, "error");
    }
  }

  async function createFolder() {
    const name = window.prompt("新文件夹名称：");
    if (name == null || name.trim() === "") return;
    const folder = name.trim();
    if (folder.includes("/") || folder.includes("\\") || folder === "." || folder === "..") {
      toast("文件夹名称不合法", "error");
      return;
    }
    try {
      await api("/api/drive/mkdir?path=" + relPath(state.cwd.concat(folder)),
        { method: "POST", body: "" });
      toast("文件夹已创建", "ok");
      loadDir(state.cwd, true);
    } catch (error) {
      toast(error.message, "error");
    }
  }

  /* ---------------- 预览 ---------------- */
  async function previewEntry(entry, segments) {
    const kind = entry.type === "directory" ? "folder" : entryKind(entry);
    if (kind === "folder") {
      loadDir(segments, false);
      return;
    }
    const modal = $("preview-modal");
    const body = $("preview-body");
    const streamUrl = entryUrl("stream", segments);
    $("preview-title").textContent = entry.name;
    body.innerHTML = "";
    modal.classList.remove("hidden");

    const showFallback = () => {
      body.innerHTML = '<div class="preview-info"><p>此文件类型暂不支持在线预览。</p>' +
        '<p><button class="btn btn-primary" id="preview-download">下载查看</button></p></div>';
      $("preview-download").addEventListener("click", () => downloadEntry(segments));
    };

    const node = document.createElement(kind === "image" ? "img" : kind === "audio" ? "audio" : "video");
    if (kind === "image" || kind === "video" || kind === "audio") {
      if (kind !== "image") {
        node.controls = true;
        if (kind === "video") node.preload = "metadata";
        node.style.maxWidth = "100%";
      } else {
        node.alt = entry.name;
      }
      node.src = streamUrl;
      if (kind !== "image") {
        node.addEventListener("error", () => {
          if (kind === "video" || kind === "audio") showFallback();
        }, { once: true });
      }
      body.appendChild(node);
      if (kind === "audio") {
        node.style.display = "block";
      }
      return;
    }
    if (kind === "pdf") {
      const frame = document.createElement("iframe");
      frame.src = streamUrl;
      body.appendChild(frame);
      return;
    }
    if (kind === "text") {
      modal.classList.add("hidden");
      if (entry.size <= MAX_EDIT_TEXT) {
        openEditor(entry, segments);
      } else {
        showFallback();
        modal.classList.remove("hidden");
      }
      return;
    }
    showFallback();
  }

  /* ---------------- 上传（分片 + 断点续传） ---------------- */
  function addUploadItem(file, folderSegments, overwrite) {
    const id = "u" + Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
    const item = {
      id,
      file,
      folder: folderSegments,
      total: file.size,
      uploaded: 0,
      overwrite: !!overwrite,
      cancelled: false,
      status: "waiting",
      error: "",
      controller: null
    };
    state.uploads.set(id, item);
    renderUploadPanel();
    state.queue.push(item);
    pumpQueue();
    return item;
  }

  function renderUploadPanel() {
    const panel = $("upload-panel");
    const list = $("upload-list");
    const hasItems = state.uploads.size > 0;
    panel.classList.toggle("hidden", !hasItems || $("view-app").classList.contains("hidden"));
    list.innerHTML = "";
    const statusText = {
      waiting: "排队中", uploading: "上传中", done: "已完成",
      skipped: "已跳过", failed: "失败"
    };
    state.uploads.forEach((item) => {
      const row = document.createElement("div");
      row.className = "upload-item";
      row.dataset.id = item.id;
      const percent = item.total > 0 ? Math.min(100, Math.round(item.uploaded * 100 / item.total)) : 100;
      const statusClass = item.status === "failed" ? "failed" :
        item.status === "skipped" ? "skipped" : item.status === "done" ? "done" : "";
      row.innerHTML =
        '<div class="upload-item-head"><span class="upload-name" title="' +
        escapeHtml(item.file.name) + '">' + escapeHtml(item.file.name) + "</span>" +
        '<button class="icon-btn" data-cancel="1" title="取消" aria-label="取消" style="width:24px;height:24px;margin:-4px 0 0 2px">×</button>' +
        '</div><div class="upload-item-head"><span class="upload-status ' + statusClass + '">' +
        (statusText[item.status] || item.status) +
        (item.error ? "：" + escapeHtml(item.error) : "") + "</span>" +
        '<span>' + formatSize(item.uploaded) + " / " + formatSize(item.total) + "</span></div>" +
        '<div class="progress"><i style="width:' + percent + '%"></i></div>';
      const cancel = row.querySelector("[data-cancel]");
      if (cancel) {
        cancel.addEventListener("click", () => {
          item.cancelled = true;
          if (item.controller) item.controller.abort();
          if (item.status === "waiting") {
            state.uploads.delete(item.id);
            state.queue = state.queue.filter((queued) => queued.id !== item.id);
            renderUploadPanel();
          } else if (item.status === "done") {
            state.uploads.delete(item.id);
            renderUploadPanel();
          } else {
            item.status = "skipped";
            item.error = "已取消";
            renderUploadPanel();
          }
        });
      }
      list.appendChild(row);
    });
  }

  function updateUploadItem(item) {
    const row = $("upload-list").querySelector('[data-id="' + item.id + '"]');
    if (row) {
      const statusMap = { waiting: "排队中", uploading: "上传中", done: "已完成", skipped: "已跳过", failed: "失败" };
      const percent = item.total > 0 ? Math.min(100, Math.round(item.uploaded * 100 / item.total)) : 100;
      const cls = item.status === "done" ? "done" : item.status === "failed" ? "failed" : item.status === "skipped" ? "skipped" : "";
      row.querySelector(".upload-status").className = "upload-status " + cls;
      row.querySelector(".upload-status").textContent = statusMap[item.status] || item.status;
      row.querySelector(".upload-item-head:nth-child(2) span:last-child").textContent =
        formatSize(item.uploaded) + " / " + formatSize(item.total);
      row.querySelector(".progress i").style.width = percent + "%";
    }
  }

  function pumpQueue() {
    while (state.running < MAX_RUNNING && state.queue.length > 0) {
      const item = state.queue.shift();
      state.running += 1;
      runUpload(item)
        .finally(() => {
          state.running -= 1;
          pumpQueue();
          const remaining = [...state.uploads.values()].some((u) => u.status === "done" ||
            u.status === "failed" || u.status === "skipped");
          const active = [...state.uploads.values()].some((u) => u.status === "waiting" ||
            u.status === "uploading");
          if (remaining && !active) {
            loadDir(state.cwd, true);
            setTimeout(() => renderUploadPanel(), 100);
          }
        });
    }
  }

  async function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }

  async function runUpload(item) {
    const file = item.file;
    const target = item.folder.concat(file.name);
    item.status = "uploading";
    updateUploadItem(item);

    try {
      // 已有同名文件时：优先续传不完整文件；完整文件询问是否覆盖
      let start = 0;
      let overwrite = item.overwrite;
      const stat = await api("/api/drive/stat?path=" + relPath(target));
      if (stat && stat.exists) {
        if (stat.type === "directory") {
          throw new ApiError(409, "同名文件夹已存在");
        }
        if (!overwrite) {
          if (stat.size > 0 && stat.size < file.size) {
            // 服务器上已有一部分数据：询问是续传还是覆盖，避免把不同内容的旧文件续坏
            const choice = window.prompt(
              "服务器上已存在同名文件（已传 " + formatSize(stat.size) +
              " / " + formatSize(file.size) + "）。\n" +
              "输入 1 断点续传，输入 2 覆盖后重新上传：", "1");
            if (choice === "2") {
              overwrite = true;
              start = 0;
            } else if (choice === "1") {
              start = stat.size;
            } else {
              item.status = "skipped";
              item.error = "已跳过";
              updateUploadItem(item);
              return;
            }
          } else {
            overwrite = window.confirm("文件「" + file.name +
              "」已存在（大小 " + formatSize(stat.size) + "）。覆盖上传？");
            if (!overwrite) {
              item.status = "skipped";
              item.error = "同名文件已存在";
              updateUploadItem(item);
              return;
            }
            start = 0;
          }
        }
      }
      item.uploaded = start;
      item.overwrite = overwrite;
      updateUploadItem(item);

      if (file.size === 0) {
        const response = await fetch("/api/drive/file/" + relPath(target), {
          method: "PATCH",
          credentials: "same-origin",
          headers: {
            "Upload-Offset": "0",
            "Content-Type": "application/octet-stream",
            ...(overwrite ? { "X-Overwrite": "1" } : {})
          },
          body: new Blob([]),
          signal: item.controller && item.controller.signal
        });
        if (!response.ok) {
          throw new ApiError(response.status, "HTTP " + response.status);
        }
        item.uploaded = 0;
        item.status = "done";
        updateUploadItem(item);
        toast("「" + file.name + "」上传完成", "ok");
        return;
      }

      while (start < file.size) {
        if (item.cancelled) {
          item.status = "skipped";
          item.error = "已取消";
          updateUploadItem(item);
          return;
        }
        const end = Math.min(file.size, start + CHUNK_SIZE);
        const chunk = file.slice(start, end);
        const headers = {
          "Upload-Offset": String(start),
          "Content-Type": "application/octet-stream"
        };
        if (overwrite && start === 0) {
          headers["X-Overwrite"] = "1";
        }
        item.controller = new AbortController();
        let response = await fetch("/api/drive/file/" + relPath(target), {
          method: "PATCH",
          credentials: "same-origin",
          headers,
          body: chunk,
          signal: item.controller.signal
        });
        if (response.status === 409 && start > 0) {
          // 服务器实际偏移不同：按服务器现状对齐，重新续传
          const fresh = await api("/api/drive/stat?path=" + relPath(target));
          if (fresh && fresh.exists && fresh.size > start) {
            start = fresh.size;
            item.uploaded = start;
            updateUploadItem(item);
            continue;
          }
          throw new ApiError(409, "文件偏移冲突");
        }
        if (!response.ok) {
          if (response.status === 401) return;
          let message = "HTTP " + response.status;
          try {
            const payload = await response.json();
            if (payload && payload.error) message = payload.error;
          } catch (ignored) { /* keep default */ }
          throw new ApiError(response.status, message);
        }
        const payload = await response.json().catch(() => null);
        const serverSize = payload && typeof payload.size === "number" ? payload.size : end;
        start = Math.max(end, serverSize);
        item.uploaded = Math.min(file.size, serverSize);
        updateUploadItem(item);
      }
      item.status = "done";
      item.error = "";
      updateUploadItem(item);
      toast("「" + file.name + "」上传完成", "ok");
    } catch (error) {
      if (item.cancelled) {
        item.status = "skipped";
        item.error = "已取消";
      } else if (error && error.name === "AbortError") {
        item.status = "skipped";
        item.error = "已取消";
      } else {
        item.status = "failed";
        item.error = error && error.message ? error.message : String(error);
        toast("「" + file.name + "」上传失败：" + item.error, "error");
      }
      updateUploadItem(item);
    }
  }

  function queueFiles(fileList, folderSegments) {
    let count = 0;
    Array.from(fileList).forEach((file) => {
      if (file.size < 0 || file.name === "") return;
      addUploadItem(file, folderSegments, false);
      count += 1;
    });
    if (count > 0) toast("已加入 " + count + " 个文件到上传队列", "ok");
    $("file-input").value = "";
    $("folder-input").value = "";
  }

  /* ---------------- 文档编辑器（文本读写） ---------------- */
  async function saveTextContent(segments, text) {
    const response = await fetch("/api/drive/write?path=" + relPath(segments), {
      method: "PUT",
      credentials: "same-origin",
      headers: { "Content-Type": "text/plain; charset=utf-8" },
      body: new Blob([text], { type: "application/octet-stream" })
    });
    if (!response.ok) {
      let message = "HTTP " + response.status;
      try {
        const payload = await response.json();
        if (payload && payload.error) message = payload.error;
      } catch (ignored) { /* 保留默认错误 */ }
      throw new ApiError(response.status, message);
    }
    return true;
  }

  function updateEditorFooter() {
    const editor = state.editor;
    const area = $("editor-textarea");
    const status = $("editor-status");
    const saveBtn = $("editor-save-btn");
    if (!editor) {
      status.textContent = "就绪";
      return;
    }
    const dirty = area.value !== editor.savedText;
    editor.text = area.value;
    editor.dirty = dirty;
    saveBtn.disabled = !dirty;
    saveBtn.textContent = dirty ? "保存 ●" : "已保存";
    const chars = area.value.length;
    status.textContent = (dirty ? "有未保存的修改 · " : "已保存 · ") +
      (editor.segments.join("/") || "根目录") + " · " + chars + " 字";
  }

  async function openEditor(entry, segments) {
    if (state.editor && state.editor.dirty &&
        !window.confirm("当前文档有未保存修改，先关闭再打开新文档？")) {
      return;
    }
    const modal = $("editor-modal");
    const area = $("editor-textarea");
    $("editor-title").textContent = entry.name;
    $("editor-path").textContent = segments.join(" / ") || "根目录";
    $("editor-preview").classList.add("hidden");
    $("editor-preview-btn").textContent = "预览";
    $("editor-body").classList.remove("split");
    modal.classList.remove("hidden");
    area.value = "正在读取文件…";
    area.readOnly = true;
    state.editor = null;
    try {
      const response = await fetch(entryUrl("stream", segments), {
        credentials: "same-origin"
      });
      if (!response.ok) throw new ApiError(response.status, "HTTP " + response.status);
      const text = await response.text();
      if (text.length > MAX_EDIT_TEXT) {
        throw new ApiError(413, "文本超过 " + formatSize(MAX_EDIT_TEXT));
      }
      state.editor = {
        name: entry.name,
        segments: segments.slice(),
        savedText: text,
        text,
        dirty: false
      };
      area.value = text;
      area.readOnly = false;
      updateEditorFooter();
      area.focus();
      area.setSelectionRange(text.length, text.length);
    } catch (error) {
      modal.classList.add("hidden");
      area.readOnly = false;
      toast("打开文档失败：" + (error.message || error), "error");
    }
  }

  function closeEditor() {
    if (!state.editor) return true;
    if (state.editor.dirty && !window.confirm("文档有未保存修改，确定关闭吗？")) {
      return false;
    }
    $("editor-modal").classList.add("hidden");
    $("editor-textarea").value = "";
    $("editor-preview").innerHTML = "";
    state.editor = null;
    updateEditorFooter();
    return true;
  }

  async function saveEditor() {
    const editor = state.editor;
    if (!editor) return;
    const area = $("editor-textarea");
    const button = $("editor-save-btn");
    button.disabled = true;
    const original = button.textContent;
    button.textContent = "保存中…";
    try {
      await saveTextContent(editor.segments, area.value);
      editor.savedText = area.value;
      editor.text = area.value;
      editor.dirty = false;
      toast("文档已保存", "ok");
      loadDir(state.cwd, true);
    } catch (error) {
      toast("保存失败：" + error.message, "error");
    } finally {
      button.textContent = original;
      updateEditorFooter();
    }
  }

  function toggleEditorPreview() {
    const wrap = $("editor-body");
    const panel = $("editor-preview");
    const button = $("editor-preview-btn");
    const on = panel.classList.contains("hidden");
    if (on) {
      panel.innerHTML = renderMarkdown(state.editor ? state.editor.text : $("editor-textarea").value);
      wrap.classList.add("split");
      button.textContent = "编辑";
    } else {
      panel.classList.add("hidden");
      wrap.classList.remove("split");
      button.textContent = "预览";
    }
  }

  async function newDocument() {
    let name = window.prompt("新文档名称（默认 .md）：", "未命名文档.md");
    if (name == null) return;
    name = name.trim();
    if (!name) return;
    if (name.includes("/") || name.includes("\\")) {
      toast("名称不能包含 / 或 \\", "error");
      return;
    }
    const ext = textFileExt(name);
    if (!TEXT_EXT.has(ext)) {
      toast("文档必须是文本格式（.md .txt .json .py …）", "error");
      return;
    }
    const dot = name.lastIndexOf(".");
    const stem = dot > 0 ? name.slice(0, dot) : name;
    let finalName = name;
    for (let i = 2; i < 200; i += 1) {
      const probe = finalName;
      try {
        const stat = await api("/api/drive/stat?path=" + relPath(state.cwd.concat(probe)));
        if (!stat || !stat.exists) break;
      } catch (error) {
        if (error.status === 403) { /* 不存在等同 */ }
        else if (error.status !== 404) throw error;
        break;
      }
      finalName = stem + " (" + i + ")" + (ext ? "." + ext : "");
    }
    const segments = state.cwd.concat(finalName);
    try {
      await saveTextContent(segments, "");
      toast("已创建「" + finalName + "」", "ok");
      await loadDir(state.cwd, true);
      await openEditor({ name: finalName, type: "file", size: 0 }, segments);
    } catch (error) {
      toast("创建文档失败：" + error.message, "error");
    }
  }

  /* ---------------- 极简 Markdown 渲染 ---------------- */
  function mdEscape(text) {
    return String(text).replace(/[&<>"']/g, (ch) => ({
      "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
    }[ch]));
  }

  function mdInline(text) {
    let out = mdEscape(text);
    out = out.replace(/`([^`\n]+)`/g, "<code>$1</code>");
    out = out.replace(/!\[([^\]]*)\]\((https?:\/\/[^)\s]+)\)/g,
      '<img src="$2" alt="$1" loading="lazy">');
    out = out.replace(/\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)/g,
      '<a href="$2" target="_blank" rel="noopener noreferrer">$1</a>');
    out = out.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
    out = out.replace(/~~([^~]+)~~/g, "<del>$1</del>");
    out = out.replace(/(^|[^\w*])\*([^*\n]+)\*/g, "$1<em>$2</em>");
    return out;
  }

  function renderMarkdown(md) {
    if (!md) return '<div class="md-doc"><p class="md-empty">空文档</p></div>';
    const lines = String(md).replace(/\r\n?/g, "\n").split("\n");
    let html = "";
    let inFence = false;
    const fence = [];
    for (let i = 0; i < lines.length; i += 1) {
      const line = lines[i];
      const fenceStart = /^```/.test(line.trim());
      if (fenceStart) {
        if (!inFence) {
          inFence = true;
          fence.length = 0;
        } else {
          inFence = false;
          html += '<pre class="md-fence"><code>' + fence.map(mdEscape).join("\n") +
            "</code></pre>";
        }
        continue;
      }
      if (inFence) {
        fence.push(line);
        continue;
      }
      if (!line.trim()) continue;
      const heading = line.match(/^(#{1,6})\s+(.*)$/);
      if (heading) {
        const level = heading[1].length;
        html += "<h" + level + ">" + mdInline(heading[2]) + "</h" + level + ">";
        continue;
      }
      if (/^\s*[-*+]\s+/.test(line)) {
        html += "<ul><li>" + mdInline(line.replace(/^\s*[-*+]\s+/, "")) + "</li></ul>";
        continue;
      }
      if (/^\s*\d+[.)]\s+/.test(line)) {
        html += "<ol><li>" + mdInline(line.replace(/^\s*\d+[.)]\s+/, "")) + "</li></ol>";
        continue;
      }
      if (/^>\s?/.test(line)) {
        html += "<blockquote>" + mdInline(line.replace(/^>\s?/, "")) + "</blockquote>";
        continue;
      }
      if (/^(\s*[-_*]\s*){3,}$/.test(line)) {
        html += "<hr>";
        continue;
      }
      let paragraph = line;
      while (i + 1 < lines.length && lines[i + 1].trim() &&
          !/^(#{1,6}\s|>\s?|\s*[-*+]\s+|\s*\d+[.)]\s+|```)/.test(lines[i + 1])) {
        paragraph += "\n" + lines[++i];
      }
      html += "<p>" + mdInline(paragraph) + "</p>";
    }
    if (inFence) {
      html += '<pre class="md-fence"><code>' + fence.map(mdEscape).join("\n") + "</code></pre>";
    }
    return '<div class="md-doc">' + html + "</div>";
  }

  /* ---------------- AI 助手 ---------------- */
  const AI_PRESETS = {
    polish: "请润色并改写以下文本，保留原有信息，让表达更清晰、自然、有文采；直接输出修改后的完整文本，不要解释。",
    fix: "请修正以下文本中的错别字和不通顺的句子，不要改变原意，直接输出完整修正结果。",
    summary: "请用中文总结以下文本的要点，使用简洁的列表输出。",
    zh: "请把以下内容翻译成中文，保留 Markdown 结构，直接输出译文。",
    en: "Please translate the following content into natural English, keep Markdown structure, and output only the translation.",
    json: "请把以下内容整理成结构清晰的 JSON，直接输出 JSON 代码块。"
  };

  let aiStatusCache = null;
  async function refreshAiStatus() {
    const chip = $("ai-model-chip");
    const status = $("ai-status");
    try {
      const payload = await api("/api/ai/status");
      aiStatusCache = payload;
      const configured = payload && payload.configured;
      chip.textContent = configured
        ? "已连接 · " + (payload.model || "")
        : "未配置 AI Key";
      if (status && !$("ai-modal").classList.contains("hidden")) {
        status.textContent = configured ? "" : "请先点击右上角齿轮配置 AI API";
      }
      return payload;
    } catch (error) {
      aiStatusCache = null;
      chip.textContent = "sidecar 未启动";
      if (status) status.textContent = error.message;
      return null;
    }
  }

  async function openAiPalette() {
    if ($("view-app").classList.contains("hidden")) return;
    state.aiResult = "";
    $("ai-prompt").value = "";
    $("ai-result-wrap").classList.add("hidden");
    $("ai-apply-btn").classList.add("hidden");
    $("ai-append-btn").classList.add("hidden");
    $("ai-copy-btn").classList.add("hidden");
    $("ai-run-btn").disabled = false;
    $("ai-status").textContent = "正在生成时请勿关闭…";

    const area = $("editor-textarea");
    let aiDoc = null;
    const editorOpen = state.editor && !$("editor-modal").classList.contains("hidden");
    if (editorOpen) {
      const start = area.selectionStart;
      const end = area.selectionEnd;
      const selected = start !== end ? area.value.slice(start, end) : "";
      aiDoc = {
        name: state.editor.name,
        segments: state.editor.segments.slice(),
        text: area.value,
        selected
      };
    }
    state.aiDoc = aiDoc;
    $("ai-context-bar").classList.toggle("hidden", !aiDoc);
    if (aiDoc) {
      $("ai-context-name").textContent = aiDoc.name +
        (aiDoc.selected ? "（选中 " + aiDoc.selected.length + " 字）" : "（整篇文档）");
    }
    $("ai-use-doc").checked = !!aiDoc;
    $("ai-modal").classList.remove("hidden");
    $("ai-prompt").focus();
    refreshAiStatus();
  }

  function closeAiPalette() {
    $("ai-modal").classList.add("hidden");
    state.aiDoc = null;
    state.aiResult = "";
    $("ai-result-wrap").classList.add("hidden");
    $("ai-run-btn").disabled = false;
  }

  function fillAiPreset(key) {
    const prompt = AI_PRESETS[key];
    if (!prompt) return;
    $("ai-prompt").value = prompt;
    $("ai-prompt").focus();
  }

  function aiContextText() {
    if (!state.aiDoc || !$("ai-use-doc").checked) return "";
    const base = state.aiDoc.selected || state.aiDoc.text || "";
    return base.slice(0, AI_MAX_CONTEXT);
  }

  async function runAi() {
    const prompt = $("ai-prompt").value.trim();
    if (!prompt) {
      toast("先输入你想让 AI 做什么", "error");
      return;
    }
    const button = $("ai-run-btn");
    const status = $("ai-status");
    button.disabled = true;
    status.textContent = "AI 思考中（大文档可能要十几秒）…";
    try {
      const context = aiContextText();
      const messages = [
        {
          role: "system",
          content: "你是藤のnetdisk 内置的写作与编辑助手。用户可能给你整篇文档或选中片段。" +
            "除非指令另有要求，请直接输出可落盘的完整结果，不要寒暄、不要加解释前缀。"
        },
        {
          role: "user",
          content: prompt +
            (context ? "\n\n以下是被编辑文档" +
              (state.aiDoc.name ? "「" + state.aiDoc.name + "」" : "") + "的内容：\n```\n" +
              context + "\n```" : "")
        }
      ];
      const payload = await api("/api/ai/chat", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ messages, temperature: 0.4, maxTokens: 4096 })
      });
      const content = (payload && payload.content) || "";
      state.aiResult = content;
      $("ai-result-text").textContent = content || "（AI 返回了空内容）";
      $("ai-result-wrap").classList.remove("hidden");
      $("ai-copy-btn").classList.remove("hidden");
      const withDoc = !!state.aiDoc;
      $("ai-apply-btn").classList.toggle("hidden", !withDoc);
      $("ai-append-btn").classList.toggle("hidden", !withDoc);
      status.textContent = payload && payload.model ? "完成 · " + payload.model : "完成";
    } catch (error) {
      status.textContent = error.message;
      $("ai-result-wrap").classList.remove("hidden");
      $("ai-result-text").textContent = "请求失败：" + error.message;
    } finally {
      button.disabled = false;
    }
  }

  async function applyAiResult(mode) {
    if (!state.aiDoc || !state.aiResult) {
      toast("请先在文档编辑器里打开一个文件", "error");
      return;
    }
    const segments = state.aiDoc.segments;
    const result = state.aiResult;
    const current = (state.editor && state.editor.name === state.aiDoc.name &&
      state.editor.segments.join("/") === segments.join("/"))
      ? state.editor.text : state.aiDoc.text;
    const next = mode === "append"
      ? ((current || "").trimEnd() ? current.trimEnd() + "\n\n" + result : result)
      : result;
    try {
      await saveTextContent(segments, next);
      if (state.editor && state.editor.name === state.aiDoc.name &&
          state.editor.segments.join("/") === segments.join("/")) {
        state.editor.savedText = next;
        state.editor.text = next;
        $("editor-textarea").value = next;
        updateEditorFooter();
      }
      toast(mode === "append" ? "AI 结果已追加到文末" : "AI 结果已保存到文档", "ok");
      loadDir(state.cwd, true);
      closeAiPalette();
    } catch (error) {
      toast("写入失败：" + error.message, "error");
    }
  }

  async function openAiConfig() {
    $("ai-config-error").classList.add("hidden");
    $("ai-config-status").textContent = "";
    const payload = await refreshAiStatus();
    $("ai-base-url").value = (payload && payload.baseUrl) || "https://api.openai.com/v1";
    $("ai-model").value = (payload && payload.model) || "";
    $("ai-api-key").value = "";
    $("ai-key-clear").checked = false;
    $("ai-config-modal").classList.remove("hidden");
  }

  function closeAiConfig() {
    $("ai-config-modal").classList.add("hidden");
  }

  async function saveAiConfig() {
    const error = $("ai-config-error");
    error.classList.add("hidden");
    const body = {
      baseUrl: $("ai-base-url").value.trim(),
      model: $("ai-model").value.trim()
    };
    const key = $("ai-api-key").value.trim();
    if (key) body.apiKey = key;
    if ($("ai-key-clear").checked) body.apiKey = "";
    if (!body.baseUrl || !body.model) {
      error.textContent = "API 地址和模型不能为空";
      error.classList.remove("hidden");
      return;
    }
    const button = $("ai-config-save");
    button.disabled = true;
    try {
      const payload = await api("/api/ai/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body)
      });
      $("ai-config-status").textContent =
        (payload && payload.configured ? "已保存并生效" : "已保存（未填写 Key）") + " · " +
        (payload && payload.model || "");
      toast("AI 配置已保存", "ok");
      refreshAiStatus();
    } catch (err) {
      error.textContent = err.message;
      error.classList.remove("hidden");
    } finally {
      button.disabled = false;
    }
  }

  async function copyAiResult() {
    if (!state.aiResult) return;
    try {
      await navigator.clipboard.writeText(state.aiResult);
      toast("已复制 AI 输出", "ok");
    } catch (error) {
      toast("复制失败，请手动选择复制", "error");
    }
  }

  /* ---------------- 第三方登录 ---------------- */
  let oauthRefreshing = null;
  function refreshOAuth() {
    if (oauthRefreshing) return oauthRefreshing;
    oauthRefreshing = (async () => {
      try {
        const payload = await api("/api/oauth/status");
        state.oauth = {
          github: !!(payload && payload.github),
          apple: !!(payload && payload.apple)
        };
      } catch (error) {
        state.oauth = { github: false, apple: false };
      } finally {
        oauthRefreshing = null;
      }
      const anyConfigured = state.oauth.github || state.oauth.apple;
      $("oauth-area").classList.toggle("hidden", !anyConfigured);
      $("oauth-github-btn").disabled = !state.oauth.github;
      $("oauth-apple-btn").disabled = !state.oauth.apple;
      $("oauth-github-btn").querySelector("span").textContent = state.oauth.github
        ? "GitHub 登录" : "GitHub 未配置";
      $("oauth-apple-btn").querySelector("span").textContent = state.oauth.apple
        ? "Apple 登录" : "Apple 未配置";
    })();
    return oauthRefreshing;
  }

  async function oauthBegin(provider) {
    const button = provider === "github" ? $("oauth-github-btn") : $("oauth-apple-btn");
    const label = button.querySelector("span");
    const original = label.textContent;
    button.disabled = true;
    label.textContent = "跳转中…";
    try {
      const response = await fetch("/api/oauth/" + provider + "/begin", {
        credentials: "same-origin"
      });
      const payload = await response.json().catch(() => null);
      if (!response.ok || !payload || !payload.authorizeUrl) {
        throw new ApiError(response.status,
          (payload && payload.error) || "第三方登录入口不可用");
      }
      window.location.assign(payload.authorizeUrl);
    } catch (error) {
      $("login-error").textContent = error.message;
      $("login-error").classList.remove("hidden");
      toast(error.message, "error");
      button.disabled = false;
      label.textContent = original;
    }
  }

  /* ---------------- 登录 ---------------- */
  async function handleLogin(event) {
    event.preventDefault();
    const username = $("login-username").value.trim();
    const password = $("login-password").value;
    const button = $("login-btn");
    const error = $("login-error");
    error.classList.add("hidden");
    button.disabled = true;
    button.querySelector(".btn-label").textContent = "登录中…";
    button.querySelector(".spinner").classList.remove("hidden");
    try {
      const payload = await api("/api/login", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username, password })
      });
      $("login-password").value = "";
      showApp(payload.username);
      toast("欢迎回来，" + payload.username, "ok");
    } catch (err) {
      error.textContent = err.message;
      error.classList.remove("hidden");
    } finally {
      button.disabled = false;
      button.querySelector(".btn-label").textContent = "登 录";
      button.querySelector(".spinner").classList.add("hidden");
    }
  }

  async function handleLogout() {
    try {
      await api("/api/logout", { method: "POST", body: "" });
    } catch (ignored) { /* cookie 可能已失效 */ }
    showLogin();
    toast("已退出登录");
  }

  async function init() {
    const viewMode = $("view-" + state.view);
    $("view-list").classList.toggle("active", state.view === "list");
    $("view-grid").classList.toggle("active", state.view === "grid");
    const params = new URLSearchParams(location.search);
    const oauthError = params.get("oauth_error");
    if (oauthError) {
      history.replaceState(null, "", location.pathname);
    }
    try {
      const me = await api("/api/me");
      showApp(me.username);
    } catch (error) {
      showLogin();
      if (oauthError) {
        const message = $("login-error");
        message.textContent = "第三方登录失败：" + oauthError;
        message.classList.remove("hidden");
      }
    }
  }

  /* ---------------- 事件绑定 ---------------- */
  function bindEvents() {
    $("login-form").addEventListener("submit", handleLogin);
    $("logout-btn").addEventListener("click", handleLogout);
    $("oauth-github-btn").addEventListener("click", () => oauthBegin("github"));
    $("oauth-apple-btn").addEventListener("click", () => oauthBegin("apple"));
    $("mkdir-btn").addEventListener("click", createFolder);
    $("new-doc-btn").addEventListener("click", newDocument);
    $("refresh-btn").addEventListener("click", () => loadDir(state.cwd, true));
    $("file-input").addEventListener("change", () => queueFiles($("file-input").files, state.cwd));
    $("folder-input").addEventListener("change", () => {
      const files = Array.from($("folder-input").files);
      const grouped = files.reduce((map, file) => {
        const parts = (file.webkitRelativePath || file.name).split("/");
        parts.pop();                       // 去掉文件名
        const dirParts = parts.filter((p) => p && p !== ".");
        map[dirParts.join("/")] = true;
        return map;
      }, {});
      if (files.length === 0) return;
      // 目录上传：先建目录，文件路径以相对目录为前缀
      const prefix = files[0].webkitRelativePath.split("/")[0];
      const folders = state.cwd.concat(prefix);
      api("/api/drive/mkdir?path=" + relPath(folders), { method: "POST", body: "" })
        .catch(() => null)
        .then(() => {
          files.forEach((file) => {
            const parts = (file.webkitRelativePath || file.name).split("/");
            parts.pop();
            const inner = parts.filter((p) => p && p !== ".").slice(1);
            addUploadItem(file, folders.concat(inner), false);
          });
          toast("已加入 " + files.length + " 个文件到上传队列", "ok");
          $("folder-input").value = "";
        });
      return;
    });

    $("view-list").addEventListener("click", () => {
      state.view = "list";
      localStorage.setItem("aurora-view", "list");
      $("view-list").classList.add("active");
      $("view-grid").classList.remove("active");
      renderEntries();
    });
    $("view-grid").addEventListener("click", () => {
      state.view = "grid";
      localStorage.setItem("aurora-view", "grid");
      $("view-grid").classList.add("active");
      $("view-list").classList.remove("active");
      renderEntries();
    });

    $("upload-clear").addEventListener("click", () => {
      [...state.uploads.values()].forEach((item) => {
        if (item.status === "done" || item.status === "skipped" || item.status === "failed") {
          state.uploads.delete(item.id);
        }
      });
      renderUploadPanel();
    });
    $("upload-toggle").addEventListener("click", () => {
      $("upload-list").classList.toggle("hidden");
    });

    $("ai-open-btn").addEventListener("click", openAiPalette);
    $("editor-ai-btn").addEventListener("click", openAiPalette);
    $("editor-preview-btn").addEventListener("click", toggleEditorPreview);
    $("editor-save-btn").addEventListener("click", saveEditor);
    $("editor-textarea").addEventListener("input", updateEditorFooter);
    $("editor-textarea").addEventListener("keydown", (event) => {
      if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "s") {
        event.preventDefault();
        saveEditor();
      }
    });
    document.querySelectorAll("#editor-modal [data-close-editor]").forEach((element) => {
      element.addEventListener("click", () => closeEditor());
    });

    $("ai-run-btn").addEventListener("click", runAi);
    $("ai-copy-btn").addEventListener("click", copyAiResult);
    $("ai-apply-btn").addEventListener("click", () => applyAiResult("replace"));
    $("ai-append-btn").addEventListener("click", () => applyAiResult("append"));
    $("ai-config-open").addEventListener("click", openAiConfig);
    $("ai-config-save").addEventListener("click", saveAiConfig);
    document.querySelectorAll("#ai-presets .chip").forEach((chip) => {
      chip.addEventListener("click", () => fillAiPreset(chip.dataset.preset));
    });
    document.querySelectorAll("#ai-modal [data-close-ai]").forEach((element) => {
      element.addEventListener("click", closeAiPalette);
    });
    document.querySelectorAll("#ai-config-modal [data-close-config]").forEach((element) => {
      element.addEventListener("click", closeAiConfig);
    });
    $("ai-prompt").addEventListener("keydown", (event) => {
      if ((event.metaKey || event.ctrlKey) && event.key === "Enter") {
        event.preventDefault();
        runAi();
      }
    });

    document.querySelectorAll("#preview-modal [data-close]").forEach((element) => {
      element.addEventListener("click", () => {
        $("preview-modal").classList.add("hidden");
        $("preview-body").innerHTML = "";
        $("preview-body").src = "";
      });
    });
    document.addEventListener("keydown", (event) => {
      const mod = event.metaKey || event.ctrlKey;
      const key = event.key.toLowerCase();
      if (mod && key === "j") {
        event.preventDefault();
        if ($("view-app").classList.contains("hidden")) return;
        if ($("ai-modal").classList.contains("hidden")) {
          openAiPalette();
        } else {
          closeAiPalette();
        }
        return;
      }
      if (event.key === "Escape") {
        if (!$("ai-config-modal").classList.contains("hidden")) {
          closeAiConfig();
        } else if (!$("ai-modal").classList.contains("hidden")) {
          closeAiPalette();
        } else if (!$("editor-modal").classList.contains("hidden")) {
          closeEditor();
        } else {
          $("preview-modal").classList.add("hidden");
          $("preview-body").innerHTML = "";
          $("preview-body").src = "";
        }
      }
    });

    // 拖拽上传
    const dropZone = $("drop-zone");
    ["dragenter", "dragover"].forEach((name) => {
      dropZone.addEventListener(name, (event) => {
        event.preventDefault();
        dropZone.classList.add("dragging");
      });
    });
    ["dragleave", "drop"].forEach((name) => {
      dropZone.addEventListener(name, (event) => {
        event.preventDefault();
        dropZone.classList.remove("dragging");
      });
    });
    dropZone.addEventListener("drop", (event) => {
      if (event.dataTransfer && event.dataTransfer.files.length) {
        queueFiles(event.dataTransfer.files, state.cwd);
      }
    });
  }

  bindEvents();
  init();
})();
