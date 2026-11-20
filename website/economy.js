document.addEventListener('DOMContentLoaded', () => {
    let token = localStorage.getItem('bronx_dash_token') || '';
    let selectedUserId = '';
    let editingItemId = '';

    const lock = document.getElementById('economy-lock');
    const app = document.getElementById('economy-app');
    const authMessage = document.getElementById('economy-auth-message');
    const message = document.getElementById('economy-message');
    const usersBody = document.getElementById('economy-users');
    const inventoryBody = document.getElementById('economy-inventory');
    const itemsBody = document.getElementById('economy-items');
    const balanceForm = document.getElementById('balance-form');
    const inventoryForm = document.getElementById('inventory-form');
    const shopForm = document.getElementById('shop-item-form');
    const profileResult = document.getElementById('profile-lookup-result');
    const profileMessage = document.getElementById('profile-lookup-message');

    function setMessage(element, text, success = false) {
        element.textContent = text;
        element.classList.toggle('success', success);
    }

    function escapeHtml(value) {
        return String(value ?? '').replace(/[&<>"']/g, char => ({
            '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
        })[char]);
    }

    function formatNumber(value) {
        const number = Number(value || 0);
        return Number.isFinite(number) ? number.toLocaleString() : '0';
    }

    async function api(url, options = {}) {
        const headers = { ...(options.headers || {}), Authorization: `Bearer ${token}` };
        if (options.body) headers['Content-Type'] = 'application/json';
        const response = await fetch(url, { ...options, headers });
        const data = await response.json().catch(() => ({}));
        if (response.status === 401) {
            token = '';
            localStorage.removeItem('bronx_dash_token');
            app.hidden = true;
            lock.hidden = false;
        }
        if (!response.ok) {
            if (response.status === 403) {
                app.hidden = true;
                lock.hidden = false;
                setMessage(authMessage, data.error || 'This panel is restricted to the bot owner.');
            }
            throw new Error(data.error || 'The request could not be completed.');
        }
        return data;
    }

    function showOwnerPanel(username) {
        lock.hidden = true;
        app.hidden = false;
        document.getElementById('economy-user').textContent = username || 'Bot owner';
        document.getElementById('economy-logout').hidden = false;
    }

    async function loadSummary() {
        const data = await api('/api/owner/economy/summary');
        const stats = data.summary;
        document.getElementById('stat-users').textContent = formatNumber(stats.user_count);
        document.getElementById('stat-wallet').textContent = formatNumber(stats.wallet_total);
        document.getElementById('stat-bank').textContent = formatNumber(stats.bank_total);
        document.getElementById('stat-items').textContent = formatNumber(stats.item_count);
        document.getElementById('stat-inventory').textContent = formatNumber(stats.inventory_count);
        document.getElementById('economy-leaders').innerHTML = data.leaders.length
            ? data.leaders.map(user => `
                <tr><td>${escapeHtml(user.user_id)}</td><td>${formatNumber(user.wallet)}</td>
                <td>${formatNumber(user.bank)}</td><td>${formatNumber(user.total)}</td></tr>
            `).join('')
            : '<tr><td colspan="4" class="economy-empty">No economy accounts yet.</td></tr>';
    }

    async function loadUsers(query = '') {
        const data = await api(`/api/owner/economy/users?q=${encodeURIComponent(query)}`);
        usersBody.innerHTML = data.users.length
            ? data.users.map(user => `
                <tr class="economy-users-row${user.user_id === selectedUserId ? ' selected' : ''}" data-user-id="${escapeHtml(user.user_id)}">
                    <td>${escapeHtml(user.user_id)}</td><td>${formatNumber(user.wallet)}</td>
                    <td>${formatNumber(user.bank)}</td><td><button class="btn btn-secondary btn-sm" type="button">Manage</button></td>
                </tr>
            `).join('')
            : '<tr><td colspan="4" class="economy-empty">No matching players.</td></tr>';

        usersBody.querySelectorAll('[data-user-id]').forEach(row => {
            row.addEventListener('click', () => selectUser(row.dataset.userId));
        });
    }

    async function lookupProfile(userId) {
        setMessage(profileMessage, '');
        profileResult.hidden = true;
        const data = await api(`/api/owner/economy/users/${encodeURIComponent(userId)}/profile`);
        document.getElementById('profile-username').textContent = data.username;
        document.getElementById('profile-user-id').textContent = data.user_id;
        document.getElementById('profile-economy-summary').textContent = data.economy
            ? `Wallet ${formatNumber(data.economy.wallet)} · Bank ${formatNumber(data.economy.bank)}`
            : 'No economy account yet.';
        const avatar = document.getElementById('profile-avatar');
        avatar.hidden = !data.avatar_url;
        if (data.avatar_url) avatar.src = data.avatar_url;
        const manageButton = document.getElementById('profile-manage-button');
        manageButton.hidden = !data.economy;
        manageButton.onclick = () => selectUser(data.user_id);
        profileResult.hidden = false;
    }

    async function selectUser(userId) {
        selectedUserId = userId;
        try {
            const data = await api(`/api/owner/economy/users?q=${encodeURIComponent(userId)}`);
            const user = data.users.find(entry => entry.user_id === userId);
            if (!user) throw new Error('Player account was not found.');

            document.getElementById('selected-player-label').textContent = userId;
            document.getElementById('balance-user-id').value = userId;
            document.getElementById('balance-wallet').value = user.wallet;
            document.getElementById('balance-bank').value = user.bank;
            document.getElementById('balance-limit').value = user.bank_limit;
            document.getElementById('balance-prestige').value = user.prestige;
            balanceForm.hidden = false;
            inventoryForm.hidden = false;
            await loadInventory();
            await loadUsers(document.getElementById('player-search').value.trim());
        } catch (err) {
            setMessage(message, err.message);
        }
    }

    async function loadInventory() {
        if (!selectedUserId) return;
        const data = await api(`/api/owner/economy/users/${encodeURIComponent(selectedUserId)}/inventory`);
        inventoryBody.innerHTML = data.items.length
            ? data.items.map(item => `
                <tr>
                    <td>${escapeHtml(item.name)}</td><td>${escapeHtml(item.item_id)}</td>
                    <td>${formatNumber(item.quantity)}</td><td>${formatNumber(item.uses_left)}</td>
                    <td>${formatNumber(item.active_until)}</td>
                    <td><div class="economy-row-actions">
                        <button class="btn btn-secondary btn-sm inventory-edit" type="button"
                            data-id="${escapeHtml(item.item_id)}" data-quantity="${item.quantity}"
                            data-uses="${item.uses_left}" data-active="${item.active_until}">Edit</button>
                        <button class="btn btn-secondary btn-sm inventory-remove" type="button"
                            data-id="${escapeHtml(item.item_id)}">Remove</button>
                    </div></td>
                </tr>
            `).join('')
            : '<tr><td colspan="6" class="economy-empty">This player has no economy items.</td></tr>';

        inventoryBody.querySelectorAll('.inventory-edit').forEach(button => {
            button.addEventListener('click', () => {
                document.getElementById('inventory-item-id').value = button.dataset.id;
                document.getElementById('inventory-quantity').value = button.dataset.quantity;
                document.getElementById('inventory-uses').value = button.dataset.uses;
                document.getElementById('inventory-active-until').value = button.dataset.active;
                inventoryForm.scrollIntoView({ behavior: 'smooth', block: 'center' });
            });
        });
        inventoryBody.querySelectorAll('.inventory-remove').forEach(button => {
            button.addEventListener('click', async () => {
                if (!confirm(`Remove ${button.dataset.id} from this player's inventory?`)) return;
                try {
                    await api(`/api/owner/economy/users/${encodeURIComponent(selectedUserId)}/inventory/${encodeURIComponent(button.dataset.id)}`, {
                        method: 'PUT',
                        body: JSON.stringify({ quantity: 0, uses_left: 0, active_until: 0 })
                    });
                    setMessage(message, 'Inventory item removed.', true);
                    await loadInventory();
                    await loadSummary();
                } catch (err) {
                    setMessage(message, err.message);
                }
            });
        });
    }

    async function loadItems() {
        const data = await api('/api/owner/economy/items');
        itemsBody.innerHTML = data.items.length
            ? data.items.map(item => `
                <tr>
                    <td>${escapeHtml(item.name)}</td><td>${escapeHtml(item.item_id)}</td>
                    <td>${escapeHtml(item.type)}</td><td>${formatNumber(item.cost)}</td>
                    <td>${formatNumber(item.uses_default)}</td>
                    <td><div class="economy-row-actions">
                        <button class="btn btn-secondary btn-sm shop-edit" type="button"
                            data-item="${escapeHtml(JSON.stringify(item))}">Edit</button>
                        <button class="btn btn-secondary btn-sm shop-delete" type="button"
                            data-id="${escapeHtml(item.item_id)}">Delete</button>
                    </div></td>
                </tr>
            `).join('')
            : '<tr><td colspan="6" class="economy-empty">No custom shop items yet.</td></tr>';

        itemsBody.querySelectorAll('.shop-edit').forEach(button => {
            button.addEventListener('click', () => editShopItem(JSON.parse(button.dataset.item)));
        });
        itemsBody.querySelectorAll('.shop-delete').forEach(button => {
            button.addEventListener('click', async () => {
                if (!confirm(`Delete the custom shop item "${button.dataset.id}"?`)) return;
                try {
                    await api(`/api/owner/economy/items/${encodeURIComponent(button.dataset.id)}`, { method: 'DELETE' });
                    resetShopForm();
                    setMessage(message, 'Shop item deleted.', true);
                    await Promise.all([loadItems(), loadSummary()]);
                } catch (err) {
                    setMessage(message, err.message);
                }
            });
        });
    }

    function editShopItem(item) {
        editingItemId = item.item_id;
        document.getElementById('shop-item-id').value = item.item_id;
        document.getElementById('shop-item-id').readOnly = true;
        document.getElementById('shop-item-name').value = item.name;
        document.getElementById('shop-item-type').value = item.type;
        document.getElementById('shop-item-cost').value = item.cost;
        document.getElementById('shop-item-uses').value = item.uses_default;
        document.getElementById('shop-item-duration').value = item.duration_seconds;
        document.getElementById('shop-item-description').value = item.description || '';
        document.getElementById('shop-form-title').textContent = 'Edit custom shop item';
        document.getElementById('shop-item-submit').textContent = 'Save changes';
        document.getElementById('shop-item-cancel').hidden = false;
        shopForm.scrollIntoView({ behavior: 'smooth', block: 'start' });
    }

    function resetShopForm() {
        editingItemId = '';
        shopForm.reset();
        document.getElementById('shop-item-id').readOnly = false;
        document.getElementById('shop-item-type').value = 'general';
        document.getElementById('shop-item-uses').value = '0';
        document.getElementById('shop-item-duration').value = '0';
        document.getElementById('shop-form-title').textContent = 'Add a custom shop item';
        document.getElementById('shop-item-submit').textContent = 'Add item';
        document.getElementById('shop-item-cancel').hidden = true;
    }

    async function loadAll() {
        setMessage(message, '');
        await Promise.all([loadSummary(), loadUsers(document.getElementById('player-search').value.trim()), loadItems()]);
    }

    document.getElementById('economy-login-form').addEventListener('submit', async event => {
        event.preventDefault();
        const accessToken = document.getElementById('economy-token').value.trim();
        setMessage(authMessage, '');
        try {
            const response = await fetch('/api/auth/login', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ token: accessToken })
            });
            const data = await response.json();
            if (!response.ok || !data.success) throw new Error(data.error || 'Sign-in failed.');
            if (!data.user?.is_owner) {
                throw new Error('This panel is restricted to the bot owner.');
            }
            token = data.token;
            localStorage.setItem('bronx_dash_token', token);
            showOwnerPanel(data.user.username);
            await loadAll();
        } catch (err) {
            setMessage(authMessage, err.message);
        }
    });

    document.getElementById('economy-logout').addEventListener('click', () => {
        token = '';
        localStorage.removeItem('bronx_dash_token');
        location.reload();
    });

    document.getElementById('economy-refresh').addEventListener('click', async () => {
        try {
            await loadAll();
            if (selectedUserId) await loadInventory();
            setMessage(message, 'Economy data refreshed.', true);
        } catch (err) {
            setMessage(message, err.message);
        }
    });

    document.getElementById('player-search-form').addEventListener('submit', async event => {
        event.preventDefault();
        try {
            await loadUsers(document.getElementById('player-search').value.trim());
        } catch (err) {
            setMessage(message, err.message);
        }
    });

    document.getElementById('profile-lookup-form').addEventListener('submit', async event => {
        event.preventDefault();
        try {
            await lookupProfile(document.getElementById('profile-lookup-id').value.trim());
        } catch (err) {
            setMessage(profileMessage, err.message);
        }
    });

    balanceForm.addEventListener('submit', async event => {
        event.preventDefault();
        const userId = document.getElementById('balance-user-id').value;
        const payload = {
            wallet: document.getElementById('balance-wallet').value,
            bank: document.getElementById('balance-bank').value,
            bank_limit: document.getElementById('balance-limit').value,
            prestige: document.getElementById('balance-prestige').value
        };
        try {
            await api(`/api/owner/economy/users/${encodeURIComponent(userId)}`, {
                method: 'PUT', body: JSON.stringify(payload)
            });
            setMessage(message, 'Player account updated.', true);
            await Promise.all([loadUsers(document.getElementById('player-search').value.trim()), loadSummary()]);
        } catch (err) {
            setMessage(message, err.message);
        }
    });

    inventoryForm.addEventListener('submit', async event => {
        event.preventDefault();
        if (!selectedUserId) return;
        const itemId = document.getElementById('inventory-item-id').value.trim();
        const payload = {
            quantity: document.getElementById('inventory-quantity').value,
            uses_left: document.getElementById('inventory-uses').value,
            active_until: document.getElementById('inventory-active-until').value
        };
        try {
            await api(`/api/owner/economy/users/${encodeURIComponent(selectedUserId)}/inventory/${encodeURIComponent(itemId)}`, {
                method: 'PUT', body: JSON.stringify(payload)
            });
            setMessage(message, 'Inventory updated.', true);
            await Promise.all([loadInventory(), loadSummary()]);
        } catch (err) {
            setMessage(message, err.message);
        }
    });

    shopForm.addEventListener('submit', async event => {
        event.preventDefault();
        const itemId = document.getElementById('shop-item-id').value.trim();
        const payload = {
            item_id: itemId,
            name: document.getElementById('shop-item-name').value.trim(),
            type: document.getElementById('shop-item-type').value.trim(),
            cost: document.getElementById('shop-item-cost').value,
            uses_default: document.getElementById('shop-item-uses').value,
            duration_seconds: document.getElementById('shop-item-duration').value,
            description: document.getElementById('shop-item-description').value.trim()
        };
        try {
            await api(editingItemId
                ? `/api/owner/economy/items/${encodeURIComponent(editingItemId)}`
                : '/api/owner/economy/items', {
                method: editingItemId ? 'PUT' : 'POST',
                body: JSON.stringify(payload)
            });
            resetShopForm();
            setMessage(message, 'Shop item saved.', true);
            await Promise.all([loadItems(), loadSummary()]);
        } catch (err) {
            setMessage(message, err.message);
        }
    });

    document.getElementById('shop-item-cancel').addEventListener('click', resetShopForm);

    document.querySelectorAll('.economy-tab').forEach(button => {
        button.addEventListener('click', () => {
            document.querySelectorAll('.economy-tab').forEach(tab => tab.classList.toggle('active', tab === button));
            document.querySelectorAll('.economy-panel').forEach(panel => {
                const active = panel.id === button.dataset.panel;
                panel.classList.toggle('active', active);
                panel.hidden = !active;
            });
        });
    });

    if (token) {
        api('/api/auth/session').then(data => {
            if (!data.authenticated) throw new Error('Dashboard token has expired. Sign in again.');
            if (!data.user?.is_owner) throw new Error('This panel is restricted to the bot owner.');
            showOwnerPanel(data.user.username);
            return loadAll();
        }).catch(err => setMessage(authMessage, err.message));
    }
});
