'use strict';

const rarityKey   = ['n', 'r', 'sr', 'ssr'];
const rarityLabel = ['N', 'R', 'SR', 'SSR'];

// クライアントが保持する「自分のインベントリ」。
// /api/sync でサーバーに丸ごとアップロードする ← この設計が脆弱。
let localInventory = [];

const rollBtn  = document.getElementById('roll-btn');
const resetBtn = document.getElementById('reset-btn');
const capsule  = document.getElementById('capsule');
const burst    = document.getElementById('burst');
const result   = document.getElementById('result');
const invEl    = document.getElementById('inventory');

async function fetchInventory() {
    const res = await fetch('/api/inventory');
    const data = await res.json();
    return data.items || [];
}

async function pushSync() {
    const ids = localInventory.map(i => i.id);
    await fetch('/api/sync', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ items: ids })
    });
}

async function rollGacha() {
    const res = await fetch('/api/roll', { method: 'POST' });
    return await res.json();
}

function sleep(ms) {
    return new Promise(r => setTimeout(r, ms));
}

async function playAnimation(item) {
    const rk = rarityKey[item.rarity];

    result.classList.remove('show', 'rarity-n', 'rarity-r', 'rarity-sr', 'rarity-ssr');
    result.classList.add('hidden');
    burst.className = '';

    capsule.className = 'spinning';
    const spinTime = item.rarity >= 2 ? 1800 : 1200;
    await sleep(spinTime);

    capsule.className = 'opening';
    burst.className = 'active rarity-' + rk;
    await sleep(300);

    result.querySelector('.rarity-badge').textContent = rarityLabel[item.rarity];
    result.querySelector('.rarity-badge').className   = 'rarity-badge r-' + rk;
    result.querySelector('.item-name').textContent    = item.name;
    result.querySelector('.item-name').className      = 'item-name r-' + rk;
    result.classList.remove('hidden');
    result.classList.add('show', 'rarity-' + rk);

    const showTime = item.rarity >= 2 ? 2200 : 1400;
    await sleep(showTime);

    capsule.className = 'idle';
    result.classList.add('hidden');
    result.classList.remove('show');
}

function renderInventory(items) {
    if (items.length === 0) {
        invEl.innerHTML = '<p class="empty">まだ何もありません</p>';
        return;
    }

    const groups = [[], [], [], []];
    items.forEach(it => groups[it.rarity].push(it));

    let html = '';
    for (let r = 3; r >= 0; r--) {
        if (groups[r].length === 0) continue;
        const counts = new Map();
        for (const it of groups[r]) {
            counts.set(it.name, (counts.get(it.name) || 0) + 1);
        }
        html += `<div class="inv-section r-${rarityKey[r]}">`;
        html += `<h3>${rarityLabel[r]}</h3><ul>`;
        for (const [name, c] of counts) {
            html += `<li><span>${escapeHtml(name)}</span><span class="count">×${c}</span></li>`;
        }
        html += `</ul></div>`;
    }
    invEl.innerHTML = html;
}

function escapeHtml(s) {
    return s.replace(/[&<>"']/g, c => ({
        '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'
    }[c]));
}

async function onRoll() {
    rollBtn.disabled = true;
    resetBtn.disabled = true;
    try {
        const item = await rollGacha();
        await playAnimation(item);

        localInventory.push(item);
        await pushSync();

        const serverItems = await fetchInventory();
        localInventory = serverItems.slice();
        renderInventory(serverItems);
    } catch (e) {
        console.error(e);
        alert('通信エラー: ' + e.message);
    } finally {
        rollBtn.disabled = false;
        resetBtn.disabled = false;
    }
}

async function onReset() {
    await fetch('/api/reset', { method: 'POST' });
    localInventory = [];
    renderInventory([]);
}

async function init() {
    const serverItems = await fetchInventory();
    localInventory = serverItems.slice();
    renderInventory(serverItems);
}

rollBtn.addEventListener('click', onRoll);
resetBtn.addEventListener('click', onReset);
init();
