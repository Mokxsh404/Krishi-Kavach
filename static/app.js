document.addEventListener('DOMContentLoaded', () => {
    // UI Elements
    const mjpegStream = document.getElementById('mjpegStream');
    const streamPlaceholder = document.getElementById('streamPlaceholder');
    const statusIndicator = document.querySelector('.status-indicator');
    
    const txtConnection = document.getElementById('txtConnection');
    const txtNgrokUrl = document.getElementById('txtNgrokUrl');
    const txtFPS = document.getElementById('txtFPS');
    const txtSpeed = document.getElementById('txtSpeed');
    const txtFrames = document.getElementById('txtFrames');
    
    const btnFullscreen = document.getElementById('btnFullscreen');
    const btnSnapshot = document.getElementById('btnSnapshot');
    const videoContainer = document.querySelector('.video-container');

    let isStreamActive = false;

    // Periodically query telemetry and status
    async function updateTelemetry() {
        try {
            const response = await fetch('/status');
            if (!response.ok) throw new Error('Network status check failed');
            const data = await response.json();
            
            // Update Ngrok URL
            if (data.ngrok_url) {
                txtNgrokUrl.textContent = data.ngrok_url;
                txtNgrokUrl.className = 'info-val text-green';
            } else {
                txtNgrokUrl.textContent = 'Offline (Run: ssh -p 443 -R0:127.0.0.1:5001 tcp@a.pinggy.io)';
                txtNgrokUrl.className = 'info-val text-red';
            }

            if (data.connected) {
                // Device connected
                txtConnection.textContent = 'Online';
                txtConnection.className = 'stat-value text-green';
                
                statusIndicator.className = 'pulse-dot status-indicator online';
                
                txtFPS.innerHTML = `${data.fps} <span class="unit">FPS</span>`;
                txtSpeed.innerHTML = `${data.kbps} <span class="unit">KB/s</span>`;
                txtFrames.textContent = data.frame_count;

                // Load stream if not loaded
                if (!isStreamActive) {
                    mjpegStream.src = '/stream';
                    mjpegStream.classList.remove('hidden');
                    streamPlaceholder.style.display = 'none';
                    isStreamActive = true;
                }
            } else {
                // Device disconnected
                txtConnection.textContent = 'Offline';
                txtConnection.className = 'stat-value text-red';
                
                statusIndicator.className = 'pulse-dot status-indicator offline';
                
                txtFPS.innerHTML = `0.0 <span class="unit">FPS</span>`;
                txtSpeed.innerHTML = `0.0 <span class="unit">KB/s</span>`;
                txtFrames.textContent = '0';

                // Stop stream
                if (isStreamActive) {
                    mjpegStream.src = '';
                    mjpegStream.classList.add('hidden');
                    streamPlaceholder.style.display = 'flex';
                    isStreamActive = false;
                }
            }
        } catch (error) {
            console.error('Error fetching telemetry:', error);
            // On fetch error, treat as offline
            txtConnection.textContent = 'Server Offline';
            txtConnection.className = 'stat-value text-red';
            statusIndicator.className = 'pulse-dot status-indicator offline';
        }
    }

    // Capture Snapshot (Saves local frame download)
    btnSnapshot.addEventListener('click', () => {
        if (!isStreamActive) {
            alert('Cannot take snapshot: Stream is offline.');
            return;
        }

        try {
            // Draw current img frame to a canvas to trigger file download
            const canvas = document.createElement('canvas');
            canvas.width = mjpegStream.naturalWidth || 320;
            canvas.height = mjpegStream.naturalHeight || 240;
            
            const ctx = canvas.getContext('2d');
            ctx.drawImage(mjpegStream, 0, 0, canvas.width, canvas.height);
            
            // Create download link
            const link = document.createElement('a');
            link.download = `gsm-snapshot-${new Date().toISOString().slice(0,19).replace(/:/g, '-')}.jpg`;
            link.href = canvas.toDataURL('image/jpeg', 0.95);
            link.click();
        } catch (err) {
            console.error('Failed to capture snapshot:', err);
            alert('Snapshot capture failed. Try again.');
        }
    });

    // Toggle Fullscreen on video element
    btnFullscreen.addEventListener('click', () => {
        if (!document.fullscreenElement) {
            if (videoContainer.requestFullscreen) {
                videoContainer.requestFullscreen();
            } else if (videoContainer.webkitRequestFullscreen) { /* Safari */
                videoContainer.webkitRequestFullscreen();
            } else if (videoContainer.msRequestFullscreen) { /* IE11 */
                videoContainer.msRequestFullscreen();
            }
        } else {
            if (document.exitFullscreen) {
                document.exitFullscreen();
            }
        }
    });

    // Handle full screen styles
    document.addEventListener('fullscreenchange', () => {
        if (document.fullscreenElement) {
            videoContainer.style.borderRadius = '0';
        } else {
            videoContainer.style.borderRadius = '12px';
        }
    });

    // Run telemetry loop every 500ms
    updateTelemetry();
    setInterval(updateTelemetry, 500);
});
