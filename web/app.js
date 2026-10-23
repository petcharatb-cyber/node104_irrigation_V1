if (!document.getElementById('chartjs-script')) {
    const chartScript = document.createElement('script');
    chartScript.id = 'chartjs-script';
    chartScript.src = "https://cdn.jsdelivr.net/npm/chart.js";
    document.head.appendChild(chartScript);
}

const FIREBASE_DB_URL = "https://smartgarden-v1-c83ce-default-rtdb.asia-southeast1.firebasedatabase.app";
const FIREBASE_AUTH = "?auth=nmPr8EM8sHuJzi3A9Pphv3qiuTREL2koLERFgCfC";
const STATE_URL = FIREBASE_DB_URL + "/node7_state.json" + FIREBASE_AUTH;
const CMD_URL = FIREBASE_DB_URL + "/node7_command.json" + FIREBASE_AUTH;
const CONFIG_URL = FIREBASE_DB_URL + "/node7_config.json" + FIREBASE_AUTH;
const HISTORY_URL = FIREBASE_DB_URL + "/node7_history.json" + FIREBASE_AUTH;

const loginSection = document.getElementById('login-section');
const dashboardSection = document.getElementById('dashboard-section');
const logicSettingSection = document.getElementById('logic-setting-section');
const loginBtn = document.getElementById('login-btn');
const logoutBtn = document.getElementById('logout-btn');

let currentRelayMask = 0; 
let overrideActive = { temp: false, hum: false, lux: false, soil: false, water: false }; 
window.ignorePollingUntil = 0; 
let myCharts = { weather: null, cultivation: null, radar: null, relay: null };
let currentHistoryData = [];

window.gardenConfig = {
    temp: { enable: true, mode: 'manual', sensor: { start: 0, stop: 0, condition: 'gte' }, allow_overwrite: false },
    hum: { enable: true, mode: 'manual', sensor: { start: 0, stop: 0, condition: 'gte' }, allow_overwrite: false },
    lux: { enable: true, mode: 'manual', sensor: { start: 0, stop: 0, condition: 'gte' }, allow_overwrite: false },
    soil: { enable: true, mode: 'manual', sensor: { start: 0, stop: 0, condition: 'gte' }, allow_overwrite: false },
    water: { enable: true, mode: 'manual', sensor: { start: 0, stop: 0, condition: 'gte' }, allow_overwrite: false }
}; 

if(loginBtn) {
    loginBtn.addEventListener('click', () => {
        if(loginSection) loginSection.classList.add('hidden');
        if(dashboardSection) dashboardSection.classList.remove('hidden');
        loadConfigFromFirebase(); 
        startFirebaseAPI_Listeners(); 
    });
}

if(logoutBtn) {
    logoutBtn.addEventListener('click', () => {
        if(loginSection) loginSection.classList.remove('hidden');
        if(dashboardSection) dashboardSection.classList.add('hidden');
    });
}

function formatTimeStr(ts) {
    if (!ts) return "-";
    const d = new Date(ts);
    const day = d.getDate().toString().padStart(2, '0');
    const months = ["ม.ค.","ก.พ.","มี.ค.","เม.ย.","พ.ค.","มิ.ย.","ก.ค.","ส.ค.","ก.ย.","ต.ค.","พ.ย.","ธ.ค."];
    const month = months[d.getMonth()];
    const hrs = d.getHours().toString().padStart(2, '0');
    const mins = d.getMinutes().toString().padStart(2, '0');
    return day + " " + month + " " + hrs + ":" + mins;
}

window.isLoggingHistory = true;
window.setLogging = function(status) {
    window.isLoggingHistory = status;
    const btnStart = document.getElementById('btn-start-log');
    const btnStop = document.getElementById('btn-stop-log');
    const statusText = document.getElementById('logging-status-text');
    
    const payload = {
        msg_type: 8,
        logging_enable: status ? 1 : 0
    };

    fetch(CMD_URL, {
        method: 'PATCH',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
    })
    .then(function(res) {
        if(!res.ok) throw new Error("API Error");
        if(btnStart && btnStop) {
            if(window.isLoggingHistory) {
                btnStart.className = "flex-1 bg-green-500 hover:bg-green-600 text-white font-bold py-2 px-2 rounded-lg shadow-sm text-sm transition-colors flex items-center justify-center gap-1";
                btnStop.className = "flex-1 bg-gray-400 hover:bg-gray-500 text-white font-bold py-2 px-2 rounded-lg shadow-sm text-sm transition-colors flex items-center justify-center gap-1";
                if(statusText) { statusText.innerText = "กำลังบันทึกข้อมูล"; statusText.className = "font-bold text-green-600 text-sm"; }
            } else {
                btnStart.className = "flex-1 bg-gray-400 hover:bg-gray-500 text-white font-bold py-2 px-2 rounded-lg shadow-sm text-sm transition-colors flex items-center justify-center gap-1";
                btnStop.className = "flex-1 bg-red-500 hover:bg-red-600 text-white font-bold py-2 px-2 rounded-lg shadow-sm text-sm transition-colors flex items-center justify-center gap-1";
                if(statusText) { statusText.innerText = "หยุดบันทึกชั่วคราว"; statusText.className = "font-bold text-red-500 text-sm"; }
            }
        }
    })
    .catch(function(err) {
        alert("เกิดข้อผิดพลาดในการส่งคำสั่ง");
    });
};

window.clearHistoryData = function() {
    if(confirm("คุณแน่ใจหรือไม่ว่าต้องการล้างข้อมูลสถิติย้อนหลังทั้งหมดบนคลาวด์? (ไม่สามารถกู้คืนได้)")) {
        fetch(HISTORY_URL, { method: 'DELETE' })
            .then(response => {
                if(response.ok) {
                    alert("ล้างข้อมูลเรียบร้อยแล้ว");
                    const countElement = document.getElementById('stat-total-rows');
                    const firstElement = document.getElementById('stat-first-date');
                    const lastElement = document.getElementById('stat-last-date');
                    if(countElement) countElement.innerText = "0";
                    if(firstElement) firstElement.innerText = "-";
                    if(lastElement) lastElement.innerText = "-";
                    Object.keys(myCharts).forEach(key => { if(myCharts[key]) myCharts[key].destroy(); });
                    currentHistoryData = [];
                } else {
                    alert("เกิดข้อผิดพลาดในการลบข้อมูล");
                }
            })
            .catch(err => {
                alert("เกิดข้อผิดพลาดในการเชื่อมต่อเครือข่าย");
            });
    }
};

function initStatsDashboardUI() {
    const statsContainer = document.querySelector('#stats-bottom-sheet .p-4') || document.getElementById('stats-bottom-sheet');
    if (!statsContainer || document.getElementById('analytics-dashboard')) return;

    const htmlParts = [
        '\x3Cdiv id="analytics-dashboard" class="w-full flex flex-col pb-20"\x3E',
        '\x3Cdiv class="flex flex-col border-b border-gray-200 pb-3 mb-4 pt-1"\x3E',
        '\x3Cdiv class="w-full flex justify-center mb-4"\x3E',
        '\x3Cdiv class="w-12 h-1.5 bg-gray-300 rounded-full cursor-pointer hover:bg-gray-400 transition-colors" onclick="window.closeStatsBottomSheet()"\x3E\x3C/div\x3E\x3C/div\x3E',
        '\x3Cdiv class="flex justify-between items-center px-2"\x3E',
        '\x3Ch2 class="text-lg sm:text-xl font-bold text-[#1e293b] flex items-center gap-2"\x3E\x3Ci class="fa-solid fa-leaf text-green-600"\x3E\x3C/i\x3E Smart Garden Node 7\x3C/h2\x3E',
        '\x3Cbutton onclick="window.closeStatsBottomSheet()" class="text-[#ef4444] hover:text-red-700 transition-colors cursor-pointer p-1"\x3E\x3Ci class="fa-solid fa-xmark text-2xl font-bold"\x3E\x3C/i\x3E\x3C/button\x3E\x3C/div\x3E\x3C/div\x3E',
        '\x3Cdiv class="px-2 mb-4"\x3E\x3Ch3 class="text-base sm:text-lg font-bold text-gray-700"\x3E\x3Ci class="fa-solid fa-chart-line text-blue-500 mr-2"\x3E\x3C/i\x3EHistorical Data Analytics\x3C/h3\x3E\x3C/div\x3E',
        
        '\x3Cdiv class="bg-blue-50 p-4 mx-2 rounded-xl shadow-sm border border-blue-100 flex flex-col sm:flex-row justify-between gap-4 mb-4"\x3E',
        '\x3Cdiv class="flex flex-col gap-2 w-full"\x3E',
        '\x3Cdiv class="flex justify-between items-center border-b border-blue-200 pb-2"\x3E\x3Cspan class="text-sm text-gray-600 font-medium"\x3E\x3Ci class="fa-regular fa-calendar-check text-green-500 mr-2"\x3E\x3C/i\x3Eข้อมูลเริ่มต้น:\x3C/span\x3E\x3Cspan id="stat-first-date" class="font-bold text-sm text-gray-800"\x3E-\x3C/span\x3E\x3C/div\x3E',
        '\x3Cdiv class="flex justify-between items-center border-b border-blue-200 pb-2"\x3E\x3Cspan class="text-sm text-gray-600 font-medium"\x3E\x3Ci class="fa-regular fa-calendar-xmark text-orange-500 mr-2"\x3E\x3C/i\x3Eข้อมูลล่าสุด:\x3C/span\x3E\x3Cspan id="stat-last-date" class="font-bold text-sm text-gray-800"\x3E-\x3C/span\x3E\x3C/div\x3E',
        '\x3Cdiv class="flex justify-between items-center pt-1"\x3E\x3Cspan class="text-sm font-semibold text-gray-700"\x3Eจำนวนบันทึกทั้งหมด:\x3C/span\x3E\x3Cspan id="stat-total-rows" class="font-bold text-lg text-blue-600"\x3E0\x3C/span\x3E\x3C/div\x3E',
        '\x3C/div\x3E\x3C/div\x3E',

        '\x3Cdiv class="bg-white p-4 mx-2 rounded-xl shadow-sm border border-gray-100 flex flex-wrap gap-4 items-end mb-4"\x3E',
        '\x3Cdiv class="flex-1 min-w-[120px]"\x3E',
        '\x3Clabel class="block text-xs font-bold text-gray-500 mb-1"\x3Eจำนวนข้อมูล (ล่าสุด)\x3C/label\x3E',
        '\x3Cselect id="hist-limit-select" class="w-full bg-gray-50 border border-gray-200 rounded-lg p-2 text-sm outline-none focus:ring-2 focus:ring-blue-400"\x3E',
        '\x3Coption value="10"\x3E10 รายการ\x3C/option\x3E\x3Coption value="30" selected\x3E30 รายการ\x3C/option\x3E\x3Coption value="50"\x3E50 รายการ\x3C/option\x3E\x3Coption value="100"\x3E100 รายการ (สูงสุด)\x3C/option\x3E\x3C/select\x3E\x3C/div\x3E',
        '\x3Cdiv class="flex-1 min-w-[150px]"\x3E\x3Clabel class="block text-xs font-bold text-gray-500 mb-1"\x3Eความถี่เก็บประวัติ (นาที)\x3C/label\x3E',
        '\x3Cdiv class="flex gap-2"\x3E\x3Cinput type="number" id="hist-interval-input" value="30" min="1" max="1440" class="w-full bg-gray-50 border border-gray-200 rounded-lg p-2 text-sm outline-none focus:ring-2 focus:ring-blue-400"\x3E',
        '\x3Cbutton onclick="updateHistoryInterval()" class="bg-blue-600 hover:bg-blue-700 text-white font-bold py-2 px-4 rounded-lg shadow-sm text-sm transition-colors"\x3EApply\x3C/button\x3E\x3C/div\x3E\x3C/div\x3E',
        
        '\x3Cdiv class="flex-1 min-w-[200px]"\x3E',
        '\x3Cdiv class="flex justify-between items-center mb-1"\x3E\x3Clabel class="block text-xs font-bold text-gray-500"\x3Eสถานะการบันทึก\x3C/label\x3E',
        '\x3Cspan id="logging-status-text" class="font-bold text-green-600 text-xs"\x3Eกำลังบันทึกข้อมูล\x3C/span\x3E\x3C/div\x3E',
        '\x3Cdiv class="flex gap-2"\x3E',
        '\x3Cbutton id="btn-start-log" onclick="setLogging(true)" class="flex-1 bg-green-500 hover:bg-green-600 text-white font-bold py-2 px-2 rounded-lg shadow-sm text-sm transition-colors flex items-center justify-center gap-1"\x3E\x3Ci class="fa-solid fa-play"\x3E\x3C/i\x3E เริ่ม\x3C/button\x3E',
        '\x3Cbutton id="btn-stop-log" onclick="setLogging(false)" class="flex-1 bg-gray-400 hover:bg-gray-500 text-white font-bold py-2 px-2 rounded-lg shadow-sm text-sm transition-colors flex items-center justify-center gap-1"\x3E\x3Ci class="fa-solid fa-stop"\x3E\x3C/i\x3E หยุด\x3C/button\x3E',
        '\x3C/div\x3E\x3C/div\x3E',

        '\x3Cdiv class="w-full flex flex-wrap gap-2 justify-end mt-2 pt-2 border-t border-gray-100"\x3E',
        '\x3Cbutton onclick="exportHistoryCSV()" class="bg-green-600 hover:bg-green-700 text-white font-bold py-2 px-4 rounded-lg shadow-sm text-sm transition-colors flex items-center gap-2"\x3E\x3Ci class="fa-solid fa-file-csv"\x3E\x3C/i\x3E Export CSV\x3C/button\x3E',
        '\x3Cbutton onclick="clearHistoryData()" class="bg-red-50 hover:bg-red-100 text-red-600 border border-red-200 font-bold py-2 px-4 rounded-lg shadow-sm text-sm transition-colors flex items-center gap-2"\x3E\x3Ci class="fa-solid fa-trash-can"\x3E\x3C/i\x3E ล้างข้อมูล\x3C/button\x3E',
        '\x3C/div\x3E',
        '\x3C/div\x3E',

        '\x3Cdiv class="grid grid-cols-1 lg:grid-cols-2 gap-4 px-2"\x3E',
        '\x3Cdiv class="bg-white p-4 rounded-xl shadow-sm border border-gray-100 h-72 relative"\x3E\x3Ch3 class="text-sm font-bold text-gray-600 mb-2"\x3Eสภาพอากาศ (Temp & Hum)\x3C/h3\x3E\x3Cdiv class="absolute inset-0 top-10 p-2"\x3E\x3Ccanvas id="chart-weather"\x3E\x3C/canvas\x3E\x3C/div\x3E\x3C/div\x3E',
        '\x3Cdiv class="bg-white p-4 rounded-xl shadow-sm border border-gray-100 h-72 relative"\x3E\x3Ch3 class="text-sm font-bold text-gray-600 mb-2"\x3Eการเพาะปลูก (Soil & Lux)\x3C/h3\x3E\x3Cdiv class="absolute inset-0 top-10 p-2"\x3E\x3Ccanvas id="chart-cultivation"\x3E\x3C/canvas\x3E\x3C/div\x3E\x3C/div\x3E',
        '\x3Cdiv class="bg-white p-4 rounded-xl shadow-sm border border-gray-100 h-80 relative flex flex-col items-center"\x3E\x3Ch3 class="text-sm font-bold text-gray-600 mb-2 w-full text-left"\x3Eความสมดุลของสภาพแวดล้อม (Correlation)\x3C/h3\x3E\x3Cdiv class="w-full max-w-[280px] h-full pb-6"\x3E\x3Ccanvas id="chart-radar"\x3E\x3C/canvas\x3E\x3C/div\x3E\x3C/div\x3E',
        '\x3Cdiv class="bg-white p-4 rounded-xl shadow-sm border border-gray-100 h-80 relative"\x3E\x3Ch3 class="text-sm font-bold text-gray-600 mb-2"\x3Eสถานะอุปกรณ์ (Relay Operations)\x3C/h3\x3E\x3Cdiv class="absolute inset-0 top-10 p-2"\x3E\x3Ccanvas id="chart-relay"\x3E\x3C/canvas\x3E\x3C/div\x3E\x3C/div\x3E\x3C/div\x3E\x3C/div\x3E'
    ];
    statsContainer.innerHTML = htmlParts.join('');
    
    const limitSelect = document.getElementById('hist-limit-select');
    if(limitSelect) limitSelect.addEventListener('change', fetchHistoricalData);
    
    loadCurrentIntervalValue();
    if(window.isLoggingHistory !== undefined) window.setLogging(window.isLoggingHistory);
    else window.setLogging(true);
}

// ฟังก์ชันดึงค่า Interval ล่าสุดจาก Firebase มาใส่ในช่อง Input
async function loadCurrentIntervalValue() {
    try {
        const res = await fetch(CMD_URL, { cache: 'no-store' });
        if(res.ok) {
            const data = await res.json();
            if(data && data.interval) {
                const intervalInput = document.getElementById('hist-interval-input');
                if(intervalInput) intervalInput.value = data.interval;
            }
        }
    } catch(e) { console.error("Error loading interval:", e); }
}

function renderCharts(dataArray) {
    if (!dataArray || dataArray.length === 0) return;
    const limitSelect = document.getElementById('hist-limit-select');
    const limit = limitSelect ? (parseInt(limitSelect.value) || 30) : 30;
    const slicedData = dataArray.slice(-limit); 
    const labels = []; const tempData = []; const humData = []; const soilData = []; const luxData = [];
    const rTemp = []; const rHum = []; const rLux = []; const rSoil = []; const rWater = [];
    let avgTemp = 0, avgHum = 0, avgSoil = 0, avgLux = 0;

    slicedData.forEach(item => {
        const d = new Date(item.timestamp);
        labels.push(d.getHours().toString().padStart(2,'0') + ":" + d.getMinutes().toString().padStart(2,'0'));
        tempData.push(item.temp || 0); avgTemp += (item.temp || 0);
        humData.push(item.humidity || item.hum || 0); avgHum += (item.humidity || item.hum || 0);
        soilData.push(item.soil_moisture || item.soil || 0); avgSoil += (item.soil_moisture || item.soil || 0);
        luxData.push(item.light_lux || item.light || item.lux || 0); avgLux += (item.light_lux || item.light || item.lux || 0);
        const mask = item.relay_mask || 0;
        rTemp.push((mask & 1) > 0 ? 1 : 0); rHum.push((mask & 2) > 0 ? 3 : 2); rLux.push((mask & 4) > 0 ? 5 : 4); rSoil.push((mask & 8) > 0 ? 7 : 6); rWater.push((mask & 16) > 0 ? 9 : 8);
    });

    const n = slicedData.length;
    avgTemp /= n; avgHum /= n; avgSoil /= n; avgLux /= n;

    Object.keys(myCharts).forEach(key => { if(myCharts[key]) myCharts[key].destroy(); });

    const ctxWeather = document.getElementById('chart-weather');
    if(ctxWeather) {
        myCharts.weather = new Chart(ctxWeather.getContext('2d'), {
            type: 'line', data: { labels: labels, datasets: [ { label: 'Temp (C)', data: tempData, borderColor: '#ef4444', backgroundColor: '#ef4444', tension: 0.4, yAxisID: 'y' }, { label: 'Humidity (%)', data: humData, borderColor: '#3b82f6', backgroundColor: '#3b82f6', tension: 0.4, yAxisID: 'y1' } ]},
            options: { responsive: true, maintainAspectRatio: false, interaction: { mode: 'index', intersect: false }, scales: { x: { ticks: { maxTicksLimit: 8 } }, y: { type: 'linear', position: 'left', min: 0, max: 60 }, y1: { type: 'linear', position: 'right', min: 0, max: 100, grid: { drawOnChartArea: false } } } }
        });
    }

    const ctxCultivation = document.getElementById('chart-cultivation');
    if(ctxCultivation) {
        myCharts.cultivation = new Chart(ctxCultivation.getContext('2d'), {
            type: 'line', data: { labels: labels, datasets: [ { label: 'Soil Moist (%)', data: soilData, borderColor: '#22c55e', backgroundColor: 'rgba(34, 197, 94, 0.2)', fill: true, tension: 0.4, yAxisID: 'y' }, { label: 'Light (Lux)', data: luxData, borderColor: '#eab308', backgroundColor: 'rgba(234, 179, 8, 0.2)', fill: true, tension: 0.4, yAxisID: 'y1' } ]},
            options: { responsive: true, maintainAspectRatio: false, interaction: { mode: 'index', intersect: false }, scales: { x: { ticks: { maxTicksLimit: 8 } }, y: { type: 'linear', position: 'left', min: 0, max: 100 }, y1: { type: 'linear', position: 'right', min: 0, grid: { drawOnChartArea: false } } } }
        });
    }

    const ctxRadar = document.getElementById('chart-radar');
    if(ctxRadar) {
        myCharts.radar = new Chart(ctxRadar.getContext('2d'), {
            type: 'radar', data: { labels: ['อุณหภูมิ', 'ความชื้นอากาศ', 'ความชื้นดิน', 'แสงสว่าง'], datasets: [{ label: 'ค่าเฉลี่ยสมดุล (Normalized %)', data: [ Math.min((avgTemp/50)*100, 100), avgHum, avgSoil, Math.min((avgLux/60000)*100, 100) ], backgroundColor: 'rgba(139, 92, 246, 0.3)', borderColor: '#8b5cf6', pointBackgroundColor: '#8b5cf6', borderWidth: 2 }] },
            options: { responsive: true, maintainAspectRatio: false, scales: { r: { min: 0, max: 100, ticks: { display: false } } } }
        });
    }

    const ctxRelay = document.getElementById('chart-relay');
    if(ctxRelay) {
        myCharts.relay = new Chart(ctxRelay.getContext('2d'), {
            type: 'line', data: { labels: labels, datasets: [ { label: 'Water Pump', data: rWater, borderColor: '#0ea5e9', backgroundColor: '#0ea5e9', stepped: true, borderWidth: 2, pointRadius: 0 }, { label: 'Soil Valve', data: rSoil, borderColor: '#22c55e', backgroundColor: '#22c55e', stepped: true, borderWidth: 2, pointRadius: 0 }, { label: 'Lux Light', data: rLux, borderColor: '#eab308', backgroundColor: '#eab308', stepped: true, borderWidth: 2, pointRadius: 0 }, { label: 'Hum Mist', data: rHum, borderColor: '#3b82f6', backgroundColor: '#3b82f6', stepped: true, borderWidth: 2, pointRadius: 0 }, { label: 'Temp Fan', data: rTemp, borderColor: '#ef4444', backgroundColor: '#ef4444', stepped: true, borderWidth: 2, pointRadius: 0 } ]},
            options: { responsive: true, maintainAspectRatio: false, interaction: { mode: 'index', intersect: false }, plugins: { tooltip: { callbacks: { label: (ctx) => { return ctx.dataset.label + ': ' + (ctx.raw % 2 === 1 ? 'ON' : 'OFF'); } } } }, scales: { x: { ticks: { maxTicksLimit: 8 } }, y: { min: -0.5, max: 10, ticks: { display: false }, grid: { display: false } } } }
        });
    }
}

async function fetchHistoricalData() {
    const countElement = document.getElementById('stat-total-rows');
    const firstElement = document.getElementById('stat-first-date');
    const lastElement = document.getElementById('stat-last-date');

    try {
        const fetchUrl = HISTORY_URL + "&_t=" + Date.now();
        const res = await fetch(fetchUrl, { cache: 'no-store' });
        if (!res.ok) throw new Error("Fetch Error");
        const data = await res.json();

        if (data) {
            currentHistoryData = Object.values(data).filter(i => i !== null).sort((a, b) => (a.timestamp || 0) - (b.timestamp || 0));
            
            if (currentHistoryData.length > 0) {
                if (countElement) countElement.innerText = currentHistoryData.length.toLocaleString();
                if (firstElement) firstElement.innerText = formatTimeStr(currentHistoryData[0].timestamp);
                if (lastElement) lastElement.innerText = formatTimeStr(currentHistoryData[currentHistoryData.length - 1].timestamp);
            } else {
                if (countElement) countElement.innerText = "0";
                if (firstElement) firstElement.innerText = "-";
                if (lastElement) lastElement.innerText = "-";
            }
            renderCharts(currentHistoryData);
        } else {
            if (countElement) countElement.innerText = "0";
            if (firstElement) firstElement.innerText = "-";
            if (lastElement) lastElement.innerText = "-";
        }
    } catch (error) { 
        console.error("Error loading history:", error); 
    }
}

window.updateHistoryInterval = async function() {
    const valInput = document.getElementById('hist-interval-input');
    if(!valInput) return;
    const val = valInput.value;
    const btn = document.querySelector('#hist-interval-input + button');
    if(btn) btn.innerHTML = '\x3Ci class="fa-solid fa-spinner fa-spin"\x3E\x3C/i\x3E';
    try {
        await fetch(CMD_URL, { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ msg_type: 7, interval: parseInt(val) }) });
        setTimeout(() => { if(btn) btn.innerHTML = 'Apply'; alert('ส่งคำสั่งตั้งเวลา (Interval) เรียบร้อยแล้ว!'); }, 1000);
    } catch (e) { if(btn) btn.innerHTML = 'Apply'; alert('Error!'); }
};

window.exportHistoryCSV = function() {
    if (!currentHistoryData || currentHistoryData.length === 0) return alert("ไม่มีข้อมูลสำหรับ Export");
    let csvContent = "data:text/csv;charset=utf-8,Datetime,Temp(C),Humidity(%),Soil(%),Lux,RelayMask\n";
    currentHistoryData.forEach(row => {
        const d = new Date(row.timestamp);
        const dateStr = d.getFullYear() + "-" + (d.getMonth()+1).toString().padStart(2,'0') + "-" + d.getDate().toString().padStart(2,'0') + " " + d.getHours().toString().padStart(2,'0') + ":" + d.getMinutes().toString().padStart(2,'0') + ":" + d.getSeconds().toString().padStart(2,'0');
        csvContent += dateStr + "," + (row.temp||0) + "," + (row.humidity||row.hum||0) + "," + (row.soil_moisture||row.soil||0) + "," + (row.light_lux||row.light||row.lux||0) + "," + (row.relay_mask||0) + "\n";
    });
    const encodedUri = encodeURI(csvContent);
    const link = document.createElement("a");
    link.setAttribute("href", encodedUri);
    link.setAttribute("download", "smart_garden_history.csv");
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
};

function evaluateSensorDot(sys, val) {
    if (val === undefined || val === null) return;
    const conf = window.gardenConfig[sys];
    if (!conf || !conf.sensor) return;
    const condition = conf.sensor.condition;
    let isActive = false;
    if (condition === 'gte') { if (val >= conf.sensor.start) isActive = true; else if (val <= conf.sensor.stop) isActive = false; } 
    else { if (val <= conf.sensor.start) isActive = true; else if (val >= conf.sensor.stop) isActive = false; }
    
    const lDot = document.getElementById("status-dot-" + sys);
    if (lDot) { 
        if (isActive && conf.enable && conf.mode !== 'manual') { lDot.classList.remove('bg-gray-400'); lDot.classList.add('bg-green-400', 'dot-blink'); } 
        else { lDot.classList.remove('bg-green-400', 'dot-blink'); lDot.classList.add('bg-gray-400'); } 
    }
    
    let s = '';
    if (sys === 'temp') s = '.border-blue-600 .w-3.h-3'; else if (sys === 'hum') s = '.border-cyan-400 .w-3.h-3'; else if (sys === 'lux') s = '.border-yellow-400 .w-3.h-3'; else if (sys === 'soil') s = '.border-green-600 .w-3.h-3';
    if (s) { 
        const mDot = document.querySelector(s); 
        if (mDot) { 
            if (isActive && conf.enable && conf.mode !== 'manual') { mDot.classList.remove('bg-gray-400'); mDot.classList.add('bg-green-500', 'dot-blink'); } 
            else { mDot.classList.remove('bg-green-500', 'dot-blink'); mDot.classList.add('bg-gray-400'); } 
        } 
    }
}

function renderBadgeUI(sys) {
    const btn = document.getElementById("btn-relay-" + sys);
    if (!btn) return;
    const container = btn.parentElement; 
    let badgeContainer = document.getElementById("dynamic-badge-" + sys);
    if (!badgeContainer) {
        badgeContainer = document.createElement('div');
        badgeContainer.id = "dynamic-badge-" + sys;
        badgeContainer.className = 'flex flex-col sm:flex-row items-start sm:items-center gap-2';
        const originalBadge = container.querySelector('span');
        if (originalBadge && originalBadge.id !== ("dynamic-badge-" + sys)) originalBadge.style.display = 'none';
        container.insertBefore(badgeContainer, container.firstChild);
    }
    const conf = window.gardenConfig[sys];
    if (!conf) return;
    if (conf.mode === 'manual') {
        badgeContainer.innerHTML = '\x3Cspan class="bg-gray-500 text-white text-xs font-bold px-3 py-1 rounded-full shadow-sm"\x3EManual Mode\x3C/span\x3E';
    } else {
        if (overrideActive[sys]) {
            badgeContainer.innerHTML = '\x3Cspan class="bg-orange-500 text-white text-xs font-bold px-3 py-1 rounded-full shadow-sm flex items-center gap-1"\x3EManual \x3Ci class="fa-solid fa-arrow-rotate-right"\x3E\x3C/i\x3E\x3C/span\x3E \x3Cbutton onclick="restoreAuto(\'' + sys + '\')" class="text-blue-600 hover:text-blue-800 text-xs font-bold underline flex items-center gap-1"\x3E\x3Ci class="fa-solid fa-rotate-left"\x3E\x3C/i\x3E คืนค่า Auto\x3C/button\x3E';
        } else {
            if (conf.allow_overwrite) badgeContainer.innerHTML = '\x3Cspan class="bg-blue-500 text-white text-xs font-bold px-3 py-1 rounded-full shadow-sm flex items-center gap-1"\x3EAuto \x3Ci class="fa-solid fa-arrow-rotate-right"\x3E\x3C/i\x3E\x3C/span\x3E';
            else badgeContainer.innerHTML = '\x3Cspan class="bg-blue-500 text-white text-xs font-bold px-3 py-1 rounded-full shadow-sm"\x3EAuto\x3C/span\x3E';
        }
    }
}

const logicBackdrop = document.createElement('div');
logicBackdrop.id = 'logic-backdrop';
logicBackdrop.className = 'fixed inset-0 bg-gray-900 bg-opacity-50 z-40 hidden opacity-0 transition-opacity duration-300 cursor-pointer backdrop-blur-sm';
if(logicSettingSection && logicSettingSection.parentNode) {
    logicSettingSection.parentNode.insertBefore(logicBackdrop, logicSettingSection);
    logicSettingSection.classList.add('fixed', 'bottom-0', 'left-0', 'right-0', 'z-50', 'bg-gray-50', 'rounded-t-[2rem]', 'shadow-[0_-10px_40px_rgba(0,0,0,0.2)]', 'h-[90vh]', 'overflow-y-auto', 'transform', 'translate-y-full', 'transition-transform', 'duration-300', 'ease-out', 'pb-10');
    logicSettingSection.classList.remove('min-h-screen', 'h-screen');

    if (!document.getElementById('bottom-sheet-handle')) {
        const handle = document.createElement('div');
        handle.id = 'bottom-sheet-handle';
        handle.className = 'sticky top-0 w-full flex justify-center pt-4 pb-3 bg-gray-50 z-20 cursor-pointer rounded-t-[2rem] border-b border-gray-200';
        handle.innerHTML = '\x3Cdiv class="w-16 h-1.5 bg-gray-300 rounded-full"\x3E\x3C/div\x3E';
        handle.addEventListener('click', () => window.closeLogicBottomSheet());
        logicSettingSection.insertBefore(handle, logicSettingSection.firstChild);
    }
}

window.openLogicBottomSheet = function() {
    if(logicSettingSection) {
        logicSettingSection.classList.remove('hidden'); 
        if(logicBackdrop) logicBackdrop.classList.remove('hidden');
        void logicSettingSection.offsetWidth;
        logicSettingSection.classList.remove('translate-y-full'); 
        if(logicBackdrop) logicBackdrop.classList.remove('opacity-0');
    }
};
window.closeLogicBottomSheet = function() {
    if(logicSettingSection) {
        logicSettingSection.classList.add('translate-y-full'); 
        if(logicBackdrop) logicBackdrop.classList.add('opacity-0');
        setTimeout(() => { 
            logicSettingSection.classList.add('hidden'); 
            if(logicBackdrop) logicBackdrop.classList.add('hidden'); 
        }, 300); 
    }
};

window.openStatsBottomSheet = function() {
    const statsBottomSheet = document.getElementById('stats-bottom-sheet');
    const statsBackdrop = document.getElementById('stats-backdrop');
    if (!statsBottomSheet || !statsBackdrop) return;
    
    initStatsDashboardUI(); 

    statsBottomSheet.classList.add('overflow-y-auto', 'h-[90vh]', 'pb-10');
    statsBottomSheet.classList.remove('h-screen', 'min-h-screen', 'overflow-hidden');
    statsBottomSheet.classList.remove('hidden'); statsBackdrop.classList.remove('hidden');
    void statsBottomSheet.offsetWidth; 
    statsBottomSheet.classList.remove('translate-y-full'); statsBackdrop.classList.remove('opacity-0');
    
    fetchHistoricalData();
};
window.closeStatsBottomSheet = function() {
    const statsBottomSheet = document.getElementById('stats-bottom-sheet');
    const statsBackdrop = document.getElementById('stats-backdrop');
    if (statsBottomSheet) statsBottomSheet.classList.add('translate-y-full'); 
    if (statsBackdrop) statsBackdrop.classList.add('opacity-0');
    setTimeout(() => { 
        if (statsBottomSheet) statsBottomSheet.classList.add('hidden'); 
        if (statsBackdrop) statsBackdrop.classList.add('hidden'); 
    }, 300); 
};

const btnOpenLogic = document.getElementById('btn-open-logic');
if(btnOpenLogic) btnOpenLogic.addEventListener('click', window.openLogicBottomSheet);

const btnBackHome = document.getElementById('btn-back-home');
if(btnBackHome) btnBackHome.addEventListener('click', window.closeLogicBottomSheet);

if(logicBackdrop) logicBackdrop.addEventListener('click', window.closeLogicBottomSheet);

const btnOpenStats = document.getElementById('btn-open-stats'); 
const btnCloseStats = document.getElementById('btn-close-stats');
const statsSheetHandle = document.getElementById('stats-sheet-handle');
const statsBackdrop = document.getElementById('stats-backdrop');

if (btnOpenStats) btnOpenStats.addEventListener('click', window.openStatsBottomSheet);
if (btnCloseStats) btnCloseStats.addEventListener('click', window.closeStatsBottomSheet);
if (statsSheetHandle) statsSheetHandle.addEventListener('click', window.closeStatsBottomSheet);
if (statsBackdrop) statsBackdrop.addEventListener('click', window.closeStatsBottomSheet);

window.addTimerRow = function(containerId) { 
    const c = document.getElementById(containerId); 
    const t = document.getElementById('timer-row-template'); 
    if(c && t) c.appendChild(t.content.cloneNode(true)); 
};
window.removeTimerRow = function(button) { 
    if(button && button.closest('.timer-row')) button.closest('.timer-row').remove(); 
};

async function loadConfigFromFirebase() {
    try {
        const res = await fetch(CONFIG_URL, { cache: 'no-store' });
        if (!res.ok) throw new Error("Cloud Error");
        const data = await res.json();
        if (data) {
            window.gardenConfig = data;
            ['temp', 'hum', 'lux', 'soil', 'water'].forEach(sys => {
                if (data[sys]) {
                    const toggle = document.getElementById("toggle-enable-" + sys);
                    if (toggle && toggle.checked !== data[sys].enable) { toggle.checked = data[sys].enable; toggle.dispatchEvent(new Event('change')); }
                    document.getElementsByName("mode-" + sys).forEach(r => { if (r.value === data[sys].mode) r.checked = true; });
                    const ow = document.getElementById("check-manual-overwrite-" + sys); if (ow) ow.checked = data[sys].allow_overwrite;
                    if (data[sys].sensor) {
                        const st = document.getElementById("sensor-start-" + sys); if(st) st.value = data[sys].sensor.start;
                        const sp = document.getElementById("sensor-stop-" + sys); if(sp) sp.value = data[sys].sensor.stop;
                        document.getElementsByName("condition-" + sys).forEach(r => { if (r.value === data[sys].sensor.condition) r.checked = true; });
                    }
                    const timerContainer = document.getElementById("timer-container-" + sys);
                    if (timerContainer) {
                        timerContainer.innerHTML = ''; 
                        if (data[sys].timers && Array.isArray(data[sys].timers)) {
                            data[sys].timers.forEach(t => {
                                window.addTimerRow("timer-container-" + sys);
                                const rows = timerContainer.querySelectorAll('.timer-row');
                                const lastRow = rows[rows.length - 1];
                                if(lastRow) {
                                    lastRow.querySelector('.timer-on').value = t.on;
                                    lastRow.querySelector('.timer-off').value = t.off;
                                }
                            });
                        }
                    }
                    renderBadgeUI(sys); 
                }
            });
        }
    } catch (error) { console.warn("ไม่สามารถดึงข้อมูล Config จาก Cloud ได้"); }
}

function startFirebaseAPI_Listeners() {
    setInterval(() => {
        fetch(STATE_URL, { cache: 'no-store' })
            .then(response => response.json())
            .then(data => {
                if (!data) return;
                const sf = document.getElementById('status-firebase');
                if (sf) { sf.innerText = "Connected"; sf.className = "text-blue-500 font-bold"; }
                const se = document.getElementById('status-esp32');
                if (se) { se.innerText = "Online"; se.className = "text-green-500 font-bold"; }
                
                const timeEl = document.getElementById('current-rtc-time');
                if (timeEl) {
                    if (data.esp_time) {
                        timeEl.innerText = "เวลาอัปเดตล่าสุด: " + data.esp_time;
                    } else {
                        const now = new Date();
                        timeEl.innerText = "เวลาอัปเดตล่าสุด: " + now.toLocaleTimeString('th-TH', { hour12: false });
                    }
                }
                
                if(document.getElementById('val-temp')) document.getElementById('val-temp').innerText = Number(data.temp || 0).toFixed(1);
                if(document.getElementById('val-hum')) document.getElementById('val-hum').innerText = Number(data.humidity || 0).toFixed(1);
                if(document.getElementById('val-lux')) document.getElementById('val-lux').innerText = Number(data.light || 0).toFixed(0);
                if(document.getElementById('val-soil')) document.getElementById('val-soil').innerText = Number(data.soil_moisture || 0).toFixed(1);
                
                currentRelayMask = data.relay_mask || 0;
                
                if (Date.now() > window.ignorePollingUntil) {
                    if(!overrideActive['temp']) { updateRelayUI('btn-relay-temp', (currentRelayMask & 1) > 0); updateLogicStatusUI('temp', (currentRelayMask & 1) > 0); }
                    if(!overrideActive['hum']) { updateRelayUI('btn-relay-hum', (currentRelayMask & 2) > 0);   updateLogicStatusUI('hum', (currentRelayMask & 2) > 0); }
                    if(!overrideActive['lux']) { updateRelayUI('btn-relay-lux', (currentRelayMask & 4) > 0);   updateLogicStatusUI('lux', (currentRelayMask & 4) > 0); }
                    if(!overrideActive['soil']) { updateRelayUI('btn-relay-soil', (currentRelayMask & 8) > 0); updateLogicStatusUI('soil', (currentRelayMask & 8) > 0); }
                    if(!overrideActive['water']) { updateRelayUI('btn-relay-water', (currentRelayMask & 16) > 0); updateLogicStatusUI('water', (currentRelayMask & 16) > 0); }
                }

                evaluateSensorDot('temp', data.temp);
                evaluateSensorDot('hum', data.humidity);
                evaluateSensorDot('lux', data.light);
                evaluateSensorDot('soil', data.soil_moisture);
            })
            .catch(error => {
                const sf = document.getElementById('status-firebase');
                if (sf) { sf.innerText = "Connection Error"; sf.className = "text-red-500 font-bold dot-blink"; }
                const se = document.getElementById('status-esp32');
                if (se) { se.innerText = "Offline / Connection Lost"; se.className = "text-red-500 font-bold dot-blink"; }
            });
    }, 2000);
}

function updateRelayUI(btnId, state) {
    const btn = document.getElementById(btnId); if (!btn) return;
    btn.classList.remove('text-green-500', 'text-red-500', 'text-gray-400');
    if (state === 'pending') btn.classList.add('text-gray-400'); 
    else if (state === true) btn.classList.add('text-green-500'); 
    else btn.classList.add('text-red-500');   
}

function updateLogicStatusUI(sys, isOn) {
    const b = document.getElementById("current-relay-status-" + sys);
    if(b) { 
        if (isOn === 'pending') {
            b.innerText = "..."; b.className = "px-3 py-1 rounded-full text-white font-bold bg-gray-400 text-sm";
        } else {
            b.innerText = isOn ? "ON" : "OFF"; b.className = isOn ? "px-3 py-1 rounded-full text-white font-bold bg-green-500 text-sm" : "px-3 py-1 rounded-full text-white font-bold bg-red-500 text-sm"; 
        }
    }
}

window.toggleRelay = function(nodeName, btnId, systemId) {
    const btn = document.getElementById(btnId); 
    if (!btn || btn.classList.contains('text-gray-400')) return; 
    
    const isCurrentlyOn = btn.classList.contains('text-green-500');
    const targetState = isCurrentlyOn ? 0 : 1;
    const relayMap = { 'temp': 0, 'hum': 1, 'lux': 2, 'soil': 3, 'water': 4 };
    const relayIdx = relayMap[systemId];
    
    if (relayIdx !== undefined) {
        const conf = window.gardenConfig[systemId];
        
        if (conf && !conf.enable) {
            alert("ไม่สามารถสั่งการได้ ระบบถูกปิดการทำงานอยู่ (Enable = OFF) กรุณาเปิดใช้งานในหน้าตั้งค่าก่อน");
            return;
        }

        if (conf && conf.mode !== 'manual' && conf.allow_overwrite) {
            overrideActive[systemId] = true;
            renderBadgeUI(systemId);
        } else if (conf && conf.mode !== 'manual' && !conf.allow_overwrite) {
             alert("ไม่สามารถสั่งการได้ กรุณาเปิดใช้งาน 'อนุญาตให้แทรกแซงการทำงานด้วยมือ' ในหน้าตั้งค่าก่อน");
             return;
        }

        updateRelayUI(btnId, 'pending');
        window.ignorePollingUntil = Date.now() + 4000;
        let newMask = currentRelayMask;
        if (targetState === 1) newMask = newMask | (1 << relayIdx);
        else newMask = newMask & ~(1 << relayIdx);

        fetch(CMD_URL, { 
            method: 'PUT', 
            headers: { 'Content-Type': 'application/json' }, 
            body: JSON.stringify({ 
                msg_type: 1, 
                relay_mask: (1 << relayIdx), 
                command_type: targetState === 1 ? 1 : 2, 
                timeout_sec: 60 
            }) 
        })
        .then(res => { 
            if(!res.ok) throw new Error("API Error");
            updateRelayUI(btnId, targetState === 1);
            currentRelayMask = newMask;
        })
        .catch(err => { alert("สั่งการไม่สำเร็จ"); updateRelayUI(btnId, isCurrentlyOn); });
    }
};

window.restoreAuto = function(sys) {
    overrideActive[sys] = false;
    renderBadgeUI(sys);
    updateRelayUI("btn-relay-" + sys, 'pending');
    updateLogicStatusUI(sys, 'pending');
    
    const relayMap = { 'temp': 0, 'hum': 1, 'lux': 2, 'soil': 3, 'water': 4 };
    
    fetch(CMD_URL, { 
        method: 'PUT', 
        headers: { 'Content-Type': 'application/json' }, 
        body: JSON.stringify({ 
            msg_type: 1, 
            relay_mask: (1 << relayMap[sys]), 
            command_type: 0, 
            timeout_sec: 60 
        }) 
    });
};

function collectSystemConfig(systemId) {
    const config = {};
    const enableToggle = document.getElementById("toggle-enable-" + systemId);
    config.enable = enableToggle ? enableToggle.checked : false;
    
    const modeRadio = document.querySelector('input[name="mode-' + systemId + '"]:checked');
    config.mode = modeRadio ? modeRadio.value : 'sensor';
    
    const overwriteCheck = document.getElementById("check-manual-overwrite-" + systemId);
    config.allow_overwrite = overwriteCheck ? overwriteCheck.checked : false;

    const startInput = document.getElementById("sensor-start-" + systemId);
    const stopInput = document.getElementById("sensor-stop-" + systemId);
    const conditionRadio = document.querySelector('input[name="condition-' + systemId + '"]:checked');
    if (startInput && stopInput) {
        config.sensor = { start: parseFloat(startInput.value) || 0, stop: parseFloat(stopInput.value) || 0, condition: conditionRadio ? conditionRadio.value : 'gte' };
    }

    const timers = [];
    const timerContainer = document.getElementById("timer-container-" + systemId);
    if (timerContainer) {
        timerContainer.querySelectorAll('.timer-row').forEach(row => {
            const onTime = row.querySelector('.timer-on').value;
            const offTime = row.querySelector('.timer-off').value;
            if (onTime && offTime) timers.push({ on: onTime, off: offTime });
        });
    }
    config.timers = timers;
    return config;
}

const btnSaveLogic = document.getElementById('btn-save-logic');
if(btnSaveLogic) {
    btnSaveLogic.addEventListener('click', async () => {
        const btn = document.getElementById('btn-save-logic');
        const originalText = btn.innerHTML;
        btn.innerHTML = '\x3Ci class="fa-solid fa-spinner fa-spin mr-2"\x3E\x3C/i\x3E กำลังอัปเดตระบบ Cloud...';
        
        try {
            const fullConfig = { temp: collectSystemConfig('temp'), hum: collectSystemConfig('hum'), lux: collectSystemConfig('lux'), soil: collectSystemConfig('soil'), water: collectSystemConfig('water') };
            const jsonString = JSON.stringify(fullConfig);

            await fetch(CONFIG_URL, { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: jsonString });

            window.gardenConfig = fullConfig;
            Object.keys(fullConfig).forEach(sys => { overrideActive[sys] = false; renderBadgeUI(sys); });

            await syncConfigToFirebase(fullConfig);
            await syncTimerToFirebase(fullConfig);

            alert("บันทึกข้อมูลและส่งไปยังสมองกลเรียบร้อยแล้ว!");
            btn.innerHTML = originalText;
        } catch (err) { alert("เกิดข้อผิดพลาดในการบันทึก"); btn.innerHTML = originalText; }
    });
}

async function syncConfigToFirebase(configObj) {
    const relayMap = { 'temp': 0, 'hum': 1, 'lux': 2, 'soil': 3, 'water': 4 };
    for (const sys of Object.keys(configObj)) {
        const conf = configObj[sys];
        let mode = 1; 
        if (conf.mode === 'manual') mode = 5; 
        else if (conf.mode === 'sensor') mode = 1; 
        else if (conf.mode === 'time') mode = 2; 
        else if (conf.mode === 'sensor_and_time') mode = 3; 
        else if (conf.mode === 'sensor_or_time') mode = 4;   
        
        await fetch(CMD_URL, {
            method: 'PUT', headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                msg_type: 5, relay_id: relayMap[sys], master_switch: conf.enable ? 1 : 0, control_mode: mode,
                sensor_start: conf.sensor ? parseFloat(conf.sensor.start) : 0, sensor_stop: conf.sensor ? parseFloat(conf.sensor.stop) : 0,
                sensor_condition: conf.sensor && conf.sensor.condition === 'lte' ? 1 : 0, allow_overwrite: conf.allow_overwrite ? 1 : 0 
            })
        });
        await new Promise(r => setTimeout(r, 2000)); 
    }
}

async function syncTimerToFirebase(configObj) {
    const relayMap = { 'temp': 0, 'hum': 1, 'lux': 2, 'soil': 3, 'water': 4 };
    for (const sys of Object.keys(configObj)) {
        const conf = configObj[sys];
        let payload = { msg_type: 6, relay_id: relayMap[sys], timer_count: 0 };
        
        if (conf.timers && conf.timers.length > 0) {
            payload.timer_count = Math.min(conf.timers.length, 5);
            for (let t = 0; t < payload.timer_count; t++) {
                const [onH, onM] = conf.timers[t].on.split(':');
                const [offH, offM] = conf.timers[t].off.split(':');
                payload["on_h_" + t] = parseInt(onH); payload["on_m_" + t] = parseInt(onM);
                payload["off_h_" + t] = parseInt(offH); payload["off_m_" + t] = parseInt(offM);
            }
        }
        
        fetch(CMD_URL, { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) });
        await new Promise(r => setTimeout(r, 2000)); 
    }
}

// ตั้งค่าให้ดึงข้อมูลอัปเดตอัตโนมัติทุกๆ 10 วินาที ตลอดเวลาที่เปิดหน้าต่างสถิติอยู่
setInterval(() => {
    const statsBottomSheet = document.getElementById('stats-bottom-sheet');
    if (statsBottomSheet && !statsBottomSheet.classList.contains('hidden')) {
        fetchHistoricalData();
    }
}, 10000);
