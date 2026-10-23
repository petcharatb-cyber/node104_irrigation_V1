importScripts('https://www.gstatic.com/firebasejs/10.8.0/firebase-app-compat.js');
importScripts('https://www.gstatic.com/firebasejs/10.8.0/firebase-messaging-compat.js');

// 1. ตั้งค่า Firebase ภายใน Service Worker
firebase.initializeApp({
    apiKey: "AIzaSyCNOVy-CC45WMs1Fp5BXZG6nVX9gGSqdBQ",
    authDomain: "smartgarden-v1-c83ce.firebaseapp.com",
    databaseURL: "https://smartgarden-v1-c83ce-default-rtdb.asia-southeast1.firebasedatabase.app",
    projectId: "smartgarden-v1-c83ce",
    storageBucket: "smartgarden-v1-c83ce.firebasestorage.app",
    messagingSenderId: "625774766209",
    appId: "1:625774766209:web:3c6319649e96f2aba6c580"
});

const messaging = firebase.messaging();

// 2. รับข้อความแจ้งเตือนตอนปิดแอป (Background)
messaging.onBackgroundMessage((payload) => {
  console.log('[firebase-messaging-sw.js] ได้รับข้อความขณะปิดแอป: ', payload);
  const notificationTitle = payload.notification.title;
  const notificationOptions = {
    body: payload.notification.body,
    icon: 'https://cdn-icons-png.flaticon.com/512/1146/1146115.png',
    badge: 'https://cdn-icons-png.flaticon.com/512/1146/1146115.png'
  };
  self.registration.showNotification(notificationTitle, notificationOptions);
});

// ==========================================
// ส่วนที่ 3: โค้ด PWA (Cache) ทำงานออฟไลน์ของเดิม
// ==========================================
//const CACHE_NAME = 'agrisystem-cache-v2';
const CACHE_NAME = 'agrisystem-cache-v3';
const urlsToCache = [
  './sgm_dashboard.html',
  './Agricultura.html',
  './manifest.json',
  'https://fonts.googleapis.com/css2?family=Prompt:wght@300;400;500;600;700&display=swap',
  'https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css',
  'https://fonts.googleapis.com/icon?family=Material+Icons'
];

self.addEventListener('install', event => {
  event.waitUntil(caches.open(CACHE_NAME).then(cache => { return cache.addAll(urlsToCache); }));
});

self.addEventListener('fetch', event => {
  event.respondWith(caches.match(event.request).then(response => { return response || fetch(event.request); }));
});

self.addEventListener('activate', event => {
  const cacheWhitelist = [CACHE_NAME];
  event.waitUntil(caches.keys().then(cacheNames => {
    return Promise.all(cacheNames.map(cacheName => {
      if (cacheWhitelist.indexOf(cacheName) === -1) { return caches.delete(cacheName); }
    }));
  }));
});
