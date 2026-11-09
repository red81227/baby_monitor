const statusEl = document.getElementById('status');
const previewEl = document.getElementById('preview');

function refreshFrame() {
  previewEl.src = `/api/frame.jpg?t=${Date.now()}`;
}

async function refreshStatus() {
  try {
    const response = await fetch('/api/status', { cache: 'no-store' });
    if (!response.ok) {
      throw new Error('status fetch failed');
    }

    const status = await response.json();
    if (status.motionDetected) {
      statusEl.textContent = 'Motion detected';
      statusEl.className = 'alert';
    } else {
      statusEl.textContent = 'No motion';
      statusEl.className = 'ok';
    }
  } catch (error) {
    statusEl.textContent = 'Backend unavailable';
    statusEl.className = 'alert';
  }
}

setInterval(refreshFrame, 250);
setInterval(refreshStatus, 1000);
refreshFrame();
refreshStatus();
