import "./style.css";

const tauri = window.__TAURI__;
const invoke = tauri.core.invoke;
const listen = tauri.event.listen;

const content = document.querySelector("#content");
const count = document.querySelector("#count");
const statusText = document.querySelector("#status");
const language = document.querySelector("#language");
const voice = document.querySelector("#voice");
const progress = document.querySelector("#progress");
const playButton = document.querySelector("#play");
const stopButton = document.querySelector("#stop");
const saveButton = document.querySelector("#save");
const closeButton = document.querySelector("#close");
const presets = [...document.querySelectorAll(".preset")];

let voices = [];
let audio = null;
let audioUrl = "";
let audioQueue = [];
let playbackJobId = 0;
let synthesisDone = false;
let jobId = 0;

function nextJob() {
  jobId += 1;
  return jobId;
}

function setBusy(busy, label = "就绪") {
  playButton.disabled = busy;
  saveButton.disabled = busy;
  stopButton.disabled = !busy && !audio;
  statusText.textContent = label;
}

function setProgress(value) {
  progress.hidden = value == null;
  progress.value = value || 0;
}

function activeVoiceCode() {
  const selected = voice.selectedOptions[0];
  return selected?.dataset.code || "zh-CN, XiaoyiNeural";
}

function setVoiceCode(code) {
  const item = voices.find((v) => v.code === code);
  if (!item) return;
  language.value = item.language;
  fillVoiceSelect(item.language, item.code);
  presets.forEach((button) => button.classList.toggle("active", button.dataset.code === code));
}

function fillLanguageSelect() {
  const languages = [...new Set(voices.map((item) => item.language))];
  language.replaceChildren(...languages.map((name) => new Option(name, name)));
}

function fillVoiceSelect(languageName, selectedCode) {
  const items = voices.filter((item) => item.language === languageName);
  voice.replaceChildren(
    ...items.map((item) => {
      const option = new Option(item.name, item.name);
      option.dataset.code = item.code;
      option.selected = item.code === selectedCode;
      return option;
    }),
  );
}

function updateCount() {
  count.textContent = `字数: ${content.value.replace(/\s+/g, "").length}`;
}

function base64ToBlob(base64) {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return new Blob([bytes], { type: "audio/mpeg" });
}

function playNextChunk(jid) {
  if (jid !== playbackJobId || audioQueue.length === 0) return;
  const { base64 } = audioQueue.shift();
  const blob = base64ToBlob(base64);
  audioUrl = URL.createObjectURL(blob);
  audio = new Audio(audioUrl);
  audio.onended = () => {
    URL.revokeObjectURL(audioUrl);
    audioUrl = "";
    audio = null;
    if (audioQueue.length > 0) {
      playNextChunk(jid);
    } else if (synthesisDone) {
      setBusy(false, "播放完成");
      setProgress(null);
    }
  };
  audio.onerror = () => {
    if (jid === playbackJobId) {
      stopAudio();
      setBusy(false, "播放失败");
      setProgress(null);
    }
  };
  audio.play();
  setBusy(true, "播放中");
  stopButton.disabled = false;
}

function stopAudio() {
  playbackJobId = 0;
  synthesisDone = false;
  audioQueue = [];
  const currentAudio = audio;
  audio = null;
  if (currentAudio) {
    currentAudio.onended = null;
    currentAudio.onerror = null;
    currentAudio.pause();
    currentAudio.removeAttribute("src");
    currentAudio.load();
  }
  if (audioUrl) {
    URL.revokeObjectURL(audioUrl);
    audioUrl = "";
  }
  stopButton.disabled = true;
}

async function stopAll() {
  nextJob();
  stopAudio();
  setBusy(false, "已停止");
  setProgress(null);
  await invoke("stop");
}

async function playText(text = content.value) {
  const trimmed = text.trim();
  if (!trimmed) return;

  stopAudio();
  synthesisDone = false;
  audioQueue = [];
  content.value = text;
  updateCount();

  const currentJob = nextJob();
  playbackJobId = currentJob;
  setBusy(true, "合成中...");
  setProgress(8);

  try {
    await invoke("synthesize_stream", {
      text,
      voice: activeVoiceCode(),
      jobId: currentJob,
    });
  } catch (error) {
    if (currentJob === jobId) {
      setBusy(false, String(error));
      setProgress(null);
    }
  }
}

async function saveText() {
  const text = content.value.trim();
  if (!text) return;

  const currentJob = nextJob();
  setBusy(true, "保存中...");
  setProgress(1);

  try {
    const saved = await invoke("save_audio", {
      text,
      voice: activeVoiceCode(),
      jobId: currentJob,
    });
    if (currentJob !== jobId) return;
    setBusy(false, saved ? `已保存: ${saved}` : "已取消保存");
    setProgress(null);
  } catch (error) {
    if (currentJob === jobId) {
      setBusy(false, String(error));
      setProgress(null);
    }
  }
}

content.addEventListener("input", updateCount);
content.addEventListener("keydown", (event) => {
  if (event.ctrlKey && event.key === "Enter") {
    event.preventDefault();
    playText();
  }
  if (event.ctrlKey && event.key.toLowerCase() === "s") {
    event.preventDefault();
    saveText();
  }
});

document.addEventListener("dragover", (event) => event.preventDefault());
document.addEventListener("drop", (event) => {
  event.preventDefault();
  const file = event.dataTransfer?.files?.[0];
  if (!file) return;
  file.text().then((text) => {
    content.value = text;
    updateCount();
  });
});

language.addEventListener("change", () => fillVoiceSelect(language.value));
voice.addEventListener("change", () => {
  const code = activeVoiceCode();
  presets.forEach((button) => button.classList.toggle("active", button.dataset.code === code));
});
presets.forEach((button) => button.addEventListener("click", () => setVoiceCode(button.dataset.code)));

playButton.addEventListener("click", () => playText());
stopButton.addEventListener("click", stopAll);
saveButton.addEventListener("click", saveText);
closeButton.addEventListener("click", () => invoke("close_window"));

listen("hotkey-text", (event) => playText(event.payload));
listen("save-progress", (event) => {
  if (event.payload.job_id === jobId) setProgress(event.payload.percent);
});
listen("playback-chunk", (event) => {
  if (event.payload.job_id !== playbackJobId) return;
  audioQueue.push({ base64: event.payload.audio_base64 });
  if (!audio) playNextChunk(playbackJobId);
});
listen("playback-done", (event) => {
  if (event.payload.job_id !== playbackJobId) return;
  synthesisDone = true;
  setProgress(null);
  if (event.payload.stopped) {
    setBusy(false, "已停止");
    return;
  }
  if (audioQueue.length === 0 && !audio) {
    setBusy(false, "播放完成");
  }
});

voices = await invoke("get_voices");
fillLanguageSelect();
setVoiceCode("zh-CN, XiaoyiNeural");
updateCount();
