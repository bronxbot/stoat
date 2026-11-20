// bronx/website/api_routes.js
// REST API routes for Bronx bot management dashboard

const express = require('express');
const router = express.Router();
const mysql = require('mysql2/promise');
const http = require('http');
const fs = require('fs');
const path = require('path');

function loadWebsiteEnvironment() {
    const envPaths = [...new Set([
        path.join(process.cwd(), '.env'),
        path.join(process.cwd(), '..', '.env'),
        path.join(__dirname, '.env'),
        path.join(__dirname, '..', '.env')
    ])];

    for (const envPath of envPaths) {
        try {
            for (const line of fs.readFileSync(envPath, 'utf8').split(/\r?\n/)) {
                if (!line || line.trimStart().startsWith('#')) continue;
                const separator = line.indexOf('=');
                if (separator < 0) continue;

                const key = line.slice(0, separator).trim();
                let value = line.slice(separator + 1).trim();
                if (value.length >= 2 && value[0] === '"' && value[value.length - 1] === '"') {
                    value = value.slice(1, -1);
                }
                if (key && process.env[key] === undefined) process.env[key] = value;
            }
        } catch (err) {
            if (err.code !== 'ENOENT') console.error(`Failed to load website environment file ${envPath}:`, err);
        }
    }
}

loadWebsiteEnvironment();

// Database Connection Pool
const pool = mysql.createPool({
    host: process.env.DB_HOST || '127.0.0.1',
    user: process.env.DB_USER || 'bronx',
    password: process.env.DB_PASSWORD || 'password',
    database: process.env.DB_DATABASE || 'bronx',
    port: parseInt(process.env.DB_PORT || '3306'),
    waitForConnections: true,
    connectionLimit: 10,
    queueLimit: 0
});

// Ensure essential website tables exist
(async () => {
    try {
        await pool.query(`
            CREATE TABLE IF NOT EXISTS dashboard_tokens (
                token VARCHAR(64) NOT NULL PRIMARY KEY,
                user_id VARCHAR(30) NOT NULL,
                user_name VARCHAR(64) DEFAULT NULL,
                created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
                expires_at TIMESTAMP NOT NULL
            );
        `);
        await pool.query(`
            CREATE TABLE IF NOT EXISTS servers (
                server_id VARCHAR(30) NOT NULL PRIMARY KEY,
                name VARCHAR(100) NOT NULL,
                icon_id VARCHAR(100) DEFAULT NULL,
                updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
            );
        `);
        await pool.query(`
            CREATE TABLE IF NOT EXISTS server_members (
                server_id VARCHAR(30) NOT NULL,
                user_id VARCHAR(30) NOT NULL,
                is_owner TINYINT(1) NOT NULL DEFAULT 0,
                is_admin TINYINT(1) NOT NULL DEFAULT 0,
                PRIMARY KEY (server_id, user_id)
            );
        `);
        await pool.query(`
            CREATE TABLE IF NOT EXISTS dashboard_allowed_users (
                server_id VARCHAR(30) NOT NULL,
                user_id VARCHAR(30) NOT NULL,
                PRIMARY KEY (server_id, user_id)
            );
        `);
        await pool.query(`
            CREATE TABLE IF NOT EXISTS dashboard_user_profiles (
                user_id VARCHAR(30) NOT NULL PRIMARY KEY,
                username VARCHAR(128) NOT NULL,
                avatar VARCHAR(100) DEFAULT NULL,
                updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
            );
        `);
    } catch (e) {
        console.error('Error initializing database tables:', e);
    }
})();

// Cache Invalidation Helper to signal C++ Bronx bot process
function notifyBotCacheReload(serverId, moduleName) {
    try {
        const req = http.request({
            hostname: '127.0.0.1',
            port: 3010,
            path: `/reload?server_id=${encodeURIComponent(serverId)}&module=${encodeURIComponent(moduleName || 'all')}`,
            method: 'GET',
            timeout: 500
        }, (res) => {});
        req.on('error', () => {}); // ignore if bot is not currently running locally
        req.end();
    } catch (e) {}
}

// -------------------------------------------------------------------
// Authentication & Session Endpoints
// -------------------------------------------------------------------

// POST /api/auth/login
router.post('/auth/login', async (req, res) => {
    const { token, secret } = req.body;
    const tokenToValidate = token || secret;
    
    if (!tokenToValidate) {
        return res.status(400).json({ error: 'Please provide your access token (generated via .dashboard token)' });
    }

    try {
        const [rows] = await pool.query(
            'SELECT user_id, user_name FROM dashboard_tokens WHERE token = ? AND expires_at > NOW()',
            [tokenToValidate]
        );

        if (rows.length === 0) {
            return res.status(401).json({ error: 'Invalid or expired access token. Please run .dashboard in any server to get a token DM.' });
        }

        const user = {
            id: rows[0].user_id,
            username: rows[0].user_name || rows[0].user_id,
            avatar: null,
            is_admin: true,
            is_owner: rows[0].user_id === getBotOwnerId()
        };

        res.json({ success: true, user, token: tokenToValidate });
    } catch (err) {
        console.error('Error validating token:', err);
        res.status(500).json({ error: 'Failed to authenticate token' });
    }
});

// GET /api/auth/session
router.get('/auth/session', async (req, res) => {
    const authHeader = req.headers.authorization;
    const token = req.query.token || (authHeader ? authHeader.replace('Bearer ', '') : null);

    if (!token) {
        return res.json({ authenticated: false });
    }

    try {
        const [rows] = await pool.query(
            'SELECT user_id, user_name FROM dashboard_tokens WHERE token = ? AND expires_at > NOW()',
            [token]
        );

        if (rows.length === 0) {
            return res.json({ authenticated: false });
        }

        res.json({
            authenticated: true,
            user: {
                id: rows[0].user_id,
                username: rows[0].user_name || rows[0].user_id,
                avatar: null,
                is_admin: true,
                is_owner: rows[0].user_id === getBotOwnerId()
            }
        });
    } catch (err) {
        res.json({ authenticated: false });
    }
});

function getBotOwnerId() {
    const configuredId = process.env.OWNER_ID || process.env.BOT_OWNER_ID;
    if (configuredId) return configuredId.trim();

    for (const envPath of [path.join(__dirname, '..', '.env'), path.join(__dirname, '.env')]) {
        try {
            const envText = fs.readFileSync(envPath, 'utf8');
            const values = {};
            for (const line of envText.split(/\r?\n/)) {
                const match = line.match(/^\s*(OWNER_ID|BOT_OWNER_ID)\s*=\s*(.*?)\s*$/);
                if (match) values[match[1]] = match[2].replace(/^["']|["']$/g, '').trim();
            }
            if (values.OWNER_ID || values.BOT_OWNER_ID) return values.OWNER_ID || values.BOT_OWNER_ID;
        } catch (err) {
            if (err.code !== 'ENOENT') {
                console.error(`Failed to read owner configuration from ${envPath}:`, err);
            }
        }
    }

    return '01FZB2QAPRVT8PVMF11480GRCD';
}

function isBotOwner(userId) {
    return userId === getBotOwnerId();
}

let botToken = process.env.BOT_TOKEN;
if (!botToken) {
    try {
        const envPath = path.join(__dirname, '../.env');
        if (fs.existsSync(envPath)) {
            const envContent = fs.readFileSync(envPath, 'utf8');
            const match = envContent.match(/BOT_TOKEN=(.+)/);
            if (match) botToken = match[1].trim();
        }
    } catch (e) {}
}

async function resolveServerDetails(serverId) {
    try {
        const [rows] = await pool.query('SELECT name, icon_id FROM servers WHERE server_id = ?', [serverId]);
        if (rows.length > 0 && rows[0].name && !rows[0].name.startsWith('Server ')) {
            return {
                name: rows[0].name,
                icon_url: rows[0].icon_id ? `https://cdn.stoatusercontent.com/icons/${rows[0].icon_id}` : null,
                owner_id: null
            };
        }

        if (botToken) {
            const fetchRes = await fetch(`https://api.stoat.chat/servers/${serverId}`, {
                headers: { 'x-bot-token': botToken }
            });
            if (fetchRes.ok) {
                const sData = await fetchRes.json();
                const name = sData.name || `Server ${serverId.slice(0, 8)}`;
                const iconId = (sData.icon && sData.icon._id) ? sData.icon._id : null;
                const ownerId = sData.owner || null;

                await pool.query(
                    `INSERT INTO servers (server_id, name, icon_id) VALUES (?, ?, ?)
                     ON DUPLICATE KEY UPDATE name=VALUES(name), icon_id=VALUES(icon_id)`,
                    [serverId, name, iconId]
                );

                if (ownerId) {
                    await pool.query(
                        `INSERT INTO server_members (server_id, user_id, is_owner, is_admin) VALUES (?, ?, 1, 1)
                         ON DUPLICATE KEY UPDATE is_owner=1, is_admin=1`,
                        [serverId, ownerId]
                    );
                }

                return {
                    name,
                    icon_url: iconId ? `https://cdn.stoatusercontent.com/icons/${iconId}` : null,
                    owner_id: ownerId
                };
            }
        }
    } catch (e) {
        console.error(`Error resolving server details for ${serverId}:`, e.message);
    }

    return {
        name: `Server ${serverId.slice(0, 8)}`,
        icon_url: null,
        owner_id: null
    };
}

async function resolveUserDetails(userId) {
    try {
        const [rows] = await pool.query(
            `SELECT username, avatar FROM dashboard_user_profiles
             WHERE user_id = ? AND updated_at > DATE_SUB(NOW(), INTERVAL 1 DAY)`,
            [userId]
        );
        if (rows.length > 0 && rows[0].username) {
            return {
                username: rows[0].username,
                avatar_url: rows[0].avatar ? `https://cdn.stoatusercontent.com/avatars/${rows[0].avatar}` : null,
                resolved: true
            };
        }
    } catch (e) {
        console.error(`Error reading cached user details for ${userId}:`, e.message);
    }

    if (!botToken) return { username: `@${userId.slice(0, 8)}`, avatar_url: null, resolved: false };

    try {
        const fetchRes = await fetch(`https://api.stoat.chat/users/${userId}`, {
            headers: { 'x-bot-token': botToken }
        });
        if (!fetchRes.ok) {
            console.error(`Stoat profile lookup for ${userId} returned HTTP ${fetchRes.status}.`);
            return { username: `@${userId.slice(0, 8)}`, avatar_url: null, resolved: false };
        }

        const uData = await fetchRes.json();
        const username = uData.username || uData.display_name || uData.name || `@${userId.slice(0, 8)}`;
        const avatarId = (uData.avatar && uData.avatar._id) ? uData.avatar._id : null;
        try {
            await pool.query(`
                INSERT INTO dashboard_user_profiles (user_id, username, avatar)
                VALUES (?, ?, ?)
                ON DUPLICATE KEY UPDATE username = VALUES(username), avatar = VALUES(avatar)
            `, [userId, username, avatarId]);
        } catch (err) {
            console.error(`Error caching user details for ${userId}:`, err.message);
        }
        return {
            username,
            avatar_url: avatarId ? `https://cdn.stoatusercontent.com/avatars/${avatarId}` : null,
            resolved: true
        };
    } catch (e) {
        console.error(`Error resolving user details for ${userId}:`, e.message);
    }
    return { username: `@${userId.slice(0, 8)}`, avatar_url: null, resolved: false };
}

async function checkServerAccess(userId, serverId) {
    // 1. Check if user is the server owner in database
    const [ownerRows] = await pool.query(
        'SELECT user_id FROM server_members WHERE server_id = ? AND is_owner = 1',
        [serverId]
    );
    let ownerId = ownerRows.length > 0 ? ownerRows[0].user_id : null;

    if (!ownerId && botToken) {
        // Fetch server details from Stoat API to resolve the owner
        try {
            const fetchRes = await fetch(`https://api.stoat.chat/servers/${serverId}`, {
                headers: { 'x-bot-token': botToken }
            });
            if (fetchRes.ok) {
                const sData = await fetchRes.json();
                ownerId = sData.owner || null;
                if (ownerId) {
                    await pool.query(
                        `INSERT INTO server_members (server_id, user_id, is_owner, is_admin) VALUES (?, ?, 1, 1)
                         ON DUPLICATE KEY UPDATE is_owner=1, is_admin=1`,
                        [serverId, ownerId]
                    );
                }
            }
        } catch (e) {
            console.error('Error fetching owner from API:', e);
        }
    }

    if (ownerId === userId) {
        return true;
    }

    // 2. Check if user is explicitly allowed in dashboard_allowed_users
    const [allowedRows] = await pool.query(
        'SELECT 1 FROM dashboard_allowed_users WHERE server_id = ? AND user_id = ?',
        [serverId, userId]
    );
    if (allowedRows.length > 0) {
        return true;
    }

    return false;
}

// Middleware to authenticate and check server access
async function requireServerAccess(req, res, next) {
    const authHeader = req.headers.authorization;
    const token = req.query.token || (authHeader ? authHeader.replace('Bearer ', '') : null);

    if (!token) {
        return res.status(401).json({ error: 'Authentication required. Please enter your dashboard access token.' });
    }

    try {
        const [tokenRows] = await pool.query(
            'SELECT user_id FROM dashboard_tokens WHERE token = ? AND expires_at > NOW()',
            [token]
        );

        if (tokenRows.length === 0) {
            return res.status(401).json({ error: 'Invalid or expired access token.' });
        }

        const userId = tokenRows[0].user_id;
        const serverId = req.params.id;

        const hasAccess = await checkServerAccess(userId, serverId);
        if (!hasAccess) {
            return res.status(403).json({ error: 'Access denied. You must be the server owner or explicitly allowed.' });
        }

        req.userId = userId;
        next();
    } catch (err) {
        console.error('Error verifying server access:', err);
        res.status(500).json({ error: 'Internal server error.' });
    }
}

// Apply access control middleware to all guild-specific paths
router.use('/guild/:id', requireServerAccess);


// GET /api/guild/:id/analytics
router.get('/guild/:id/analytics', async (req, res) => {
    const { id } = req.params;
    try {
        // 1. Top 10 Human Members by XP or Activity (excluding bots)
        let [xpRows] = await pool.query(`
            SELECT u.user_id, u.total_xp, u.level, m.username, m.avatar
            FROM user_xp u
            LEFT JOIN server_members m ON m.user_id = u.user_id AND m.server_id = u.server_id
            WHERE u.server_id = ? AND u.user_id NOT IN (SELECT DISTINCT user_id FROM guild_user_activity_daily WHERE is_bot = 1)
            ORDER BY u.total_xp DESC
            LIMIT 10
        `, [id]);

        if (xpRows.length === 0) {
            // Check guild_user_activity_daily for this guild excluding bots (is_bot = 0)
            const [actRows] = await pool.query(`
                SELECT a.user_id, SUM(a.messages) AS total_xp, 1 AS level, m.username, m.avatar
                FROM guild_user_activity_daily a
                LEFT JOIN server_members m ON m.user_id = a.user_id AND m.server_id = a.guild_id
                WHERE a.guild_id = ? AND a.user_id != '' AND a.is_bot = 0
                GROUP BY a.user_id
                ORDER BY total_xp DESC
                LIMIT 10
            `, [id]);

            if (actRows.length > 0) {
                xpRows = actRows;
            } else {
                // Global top active human members across all servers fallback
                const [globalXpRows] = await pool.query(`
                    SELECT u.user_id, SUM(u.total_xp) AS total_xp, MAX(u.level) AS level, m.username, m.avatar
                    FROM user_xp u
                    LEFT JOIN server_members m ON m.user_id = u.user_id
                    WHERE u.user_id NOT IN (SELECT DISTINCT user_id FROM guild_user_activity_daily WHERE is_bot = 1)
                    GROUP BY u.user_id
                    ORDER BY total_xp DESC
                    LIMIT 10
                `);
                xpRows = globalXpRows;
            }
        }

        const topMembers = await Promise.all(xpRows.map(async (row) => {
            let name = row.username;
            let avatarUrl = row.avatar ? `https://cdn.stoatusercontent.com/avatars/${row.avatar}` : null;
            if (!name || name === row.user_id) {
                const resolved = await resolveUserDetails(row.user_id);
                name = resolved.username;
                avatarUrl = resolved.avatar_url || avatarUrl;
            }
            return {
                user_id: row.user_id,
                username: name || `@${row.user_id.slice(0, 8)}`,
                avatar_url: avatarUrl,
                xp: parseInt(row.total_xp) || 0,
                level: parseInt(row.level) || 1
            };
        }));

        // 2. Summary Counters
        const [[memberCountRow]] = await pool.query('SELECT COUNT(DISTINCT user_id) AS total_members FROM server_members WHERE server_id = ?', [id]);
        const [[xpTotalRow]] = await pool.query('SELECT SUM(total_xp) AS total_xp, COUNT(*) AS total_xp_users FROM user_xp WHERE server_id = ?', [id]);
        const [[modCountRow]] = await pool.query('SELECT COUNT(*) AS total_mod_actions FROM moderation_logs WHERE server_id = ?', [id]);
        const [[arCountRow]] = await pool.query('SELECT COUNT(*) AS total_autoresponders FROM autoresponders WHERE server_id = ?', [id]);
        const [[areCountRow]] = await pool.query('SELECT COUNT(*) AS total_autoreacts FROM autoreacts WHERE server_id = ?', [id]);

        // 3. Top Most Used Commands from guild_command_usage
        const [modBreakdownRows] = await pool.query(`
            SELECT action, COUNT(*) AS count
            FROM moderation_logs
            WHERE server_id = ?
            GROUP BY action
        `, [id]);

        const [cmdUsageRows] = await pool.query(`
            SELECT command_name AS command, SUM(use_count) AS count
            FROM guild_command_usage
            WHERE guild_id = ?
            GROUP BY command_name
            ORDER BY count DESC
            LIMIT 10
        `, [id]);

        let commandUsage = cmdUsageRows.map(r => ({
            command: `.${r.command}`,
            count: parseInt(r.count) || 0
        }));

        if (commandUsage.length === 0) {
            // Global fallback top commands if guild specific usage hasn't recorded yet
            const [globalCmdRows] = await pool.query(`
                SELECT command_name AS command, SUM(use_count) AS count
                FROM guild_command_usage
                GROUP BY command_name
                ORDER BY count DESC
                LIMIT 10
            `);
            commandUsage = globalCmdRows.map(r => ({
                command: `.${r.command}`,
                count: parseInt(r.count) || 0
            }));
        }

        res.json({
            stats: {
                total_members: memberCountRow?.total_members || topMembers.length,
                total_xp: xpTotalRow?.total_xp || 0,
                active_users: topMembers.length,
                mod_actions: modCountRow?.total_mod_actions || 0
            },
            top_members: topMembers,
            command_usage: commandUsage,
            mod_breakdown: modBreakdownRows
        });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// GET /api/user/guilds
router.get('/user/guilds', async (req, res) => {
    const authHeader = req.headers.authorization;
    const token = req.query.token || (authHeader ? authHeader.replace('Bearer ', '') : null);

    if (!token) {
        return res.status(401).json({ error: 'Authentication required. Please enter your dashboard access token.' });
    }

    try {
        const [tokenRows] = await pool.query(
            'SELECT user_id, user_name FROM dashboard_tokens WHERE token = ? AND expires_at > NOW()',
            [token]
        );

        if (tokenRows.length === 0) {
            return res.status(401).json({ error: 'Invalid or expired access token.' });
        }

        const userId = tokenRows[0].user_id;

        // 1. Query candidate servers
        const [allServerRows] = await pool.query(`
            SELECT DISTINCT server_id FROM (
                SELECT server_id FROM server_members WHERE user_id = ?
                UNION SELECT server_id FROM trusted_entities WHERE entity_type='user' AND entity_id = ?
                UNION SELECT server_id FROM user_xp WHERE user_id = ? AND server_id != '0'
                UNION SELECT server_id FROM welcome_leave_configs
                UNION SELECT server_id FROM logging_configs
                UNION SELECT server_id FROM antiraid_configs
                UNION SELECT target_id AS server_id FROM prefixes WHERE target_type='server'
                UNION SELECT server_id FROM dashboard_allowed_users WHERE user_id = ?
            ) AS s_id WHERE server_id IS NOT NULL AND server_id != ''
        `, [userId, userId, userId, userId]);

        // 2. Resolve names, icons, and ownership for each server
        const guilds = [];
        for (const row of allServerRows) {
            const serverId = row.server_id;
            const hasAccess = await checkServerAccess(userId, serverId);

            if (hasAccess) {
                const details = await resolveServerDetails(serverId);
                // Also double check if they are the actual owner (for frontend UI to show ownership icon if needed)
                const [ownerCheck] = await pool.query(
                    'SELECT 1 FROM server_members WHERE server_id = ? AND user_id = ? AND is_owner = 1',
                    [serverId, userId]
                );
                const isOwner = ownerCheck.length > 0;

                guilds.push({
                    id: serverId,
                    name: details.name,
                    icon: details.icon_url,
                    owner: isOwner,
                    permissions: ['ManageServer', 'Administrator']
                });
            }
        }

        res.json({
            guilds,
            user: {
                id: userId,
                username: tokenRows[0].user_name,
                is_owner: isBotOwner(userId)
            }
        });
    } catch (err) {
        console.error('Error fetching user guilds:', err);
        res.status(500).json({ error: 'Failed to fetch guilds' });
    }
});

// -------------------------------------------------------------------
// 1. Welcome & Leave Configs
// -------------------------------------------------------------------

router.get('/guild/:id/welcome-leave', async (req, res) => {
    const { id } = req.params;
    try {
        const [rows] = await pool.query(
            'SELECT * FROM welcome_leave_configs WHERE server_id = ?',
            [id]
        );
        if (rows.length === 0) {
            return res.json({
                server_id: id,
                welcome_channel_id: '',
                welcome_type: 'text',
                welcome_content: 'Welcome to **{server}**, {user}!',
                welcome_embed_json: '',
                leave_channel_id: '',
                leave_type: 'text',
                leave_content: '{username} left the server.',
                leave_embed_json: '',
                autorole_role_id: '',
                welcome_dm_type: 'none',
                welcome_dm_content: '',
                welcome_dm_embed_json: ''
            });
        }
        const config = rows[0];
        res.json({
            ...config,
            welcome_embed_json: typeof config.welcome_embed === 'object' ? (config.welcome_embed ? JSON.stringify(config.welcome_embed, null, 2) : '') : (config.welcome_embed || ''),
            leave_embed_json: typeof config.leave_embed === 'object' ? (config.leave_embed ? JSON.stringify(config.leave_embed, null, 2) : '') : (config.leave_embed || ''),
            welcome_dm_embed_json: typeof config.welcome_dm_embed === 'object' ? (config.welcome_dm_embed ? JSON.stringify(config.welcome_dm_embed, null, 2) : '') : (config.welcome_dm_embed || '')
        });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/welcome-leave', async (req, res) => {
    const { id } = req.params;
    const {
        welcome_channel_id, welcome_type, welcome_content, welcome_embed_json,
        leave_channel_id, leave_type, leave_content, leave_embed_json,
        autorole_role_id, welcome_dm_type, welcome_dm_content, welcome_dm_embed_json
    } = req.body;

    try {
        await pool.query(`
            INSERT INTO welcome_leave_configs 
            (server_id, welcome_channel_id, welcome_type, welcome_content, welcome_embed,
             leave_channel_id, leave_type, leave_content, leave_embed, autorole_role_id,
             welcome_dm_type, welcome_dm_content, welcome_dm_embed)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON DUPLICATE KEY UPDATE
            welcome_channel_id=VALUES(welcome_channel_id), welcome_type=VALUES(welcome_type),
            welcome_content=VALUES(welcome_content), welcome_embed=VALUES(welcome_embed),
            leave_channel_id=VALUES(leave_channel_id), leave_type=VALUES(leave_type),
            leave_content=VALUES(leave_content), leave_embed=VALUES(leave_embed),
            autorole_role_id=VALUES(autorole_role_id), welcome_dm_type=VALUES(welcome_dm_type),
            welcome_dm_content=VALUES(welcome_dm_content), welcome_dm_embed=VALUES(welcome_dm_embed)
        `, [
            id, welcome_channel_id || null, welcome_type || 'text', welcome_content || null, welcome_embed_json || null,
            leave_channel_id || null, leave_type || 'text', leave_content || null, leave_embed_json || null,
            autorole_role_id || null, welcome_dm_type || 'none', welcome_dm_content || null, welcome_dm_embed_json || null
        ]);

        notifyBotCacheReload(id, 'welcome_leave');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// -------------------------------------------------------------------
// 2. Autoresponders & Autoreacts
// -------------------------------------------------------------------

router.get('/guild/:id/emojis', async (req, res) => {
    const { id } = req.params;
    try {
        if (botToken) {
            const fetchRes = await fetch(`https://api.stoat.chat/servers/${id}`, {
                headers: { 'x-bot-token': botToken }
            });
            if (fetchRes.ok) {
                const sData = await fetchRes.json();
                const emojis = sData.emojis || [];
                const formatted = emojis.map(e => ({
                    id: e._id,
                    name: e.name,
                    url: `https://cdn.stoatusercontent.com/emojis/${e._id}`,
                    code: `:${e.name}:`
                }));
                return res.json(formatted);
            }
        }
        res.json([]);
    } catch (err) {
        res.json([]);
    }
});

router.get('/guild/:id/autoresponders', async (req, res) => {
    const { id } = req.params;
    try {
        const [rows] = await pool.query('SELECT * FROM autoresponders WHERE server_id = ? ORDER BY id DESC', [id]);
        res.json(rows);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/autoresponders', async (req, res) => {
    const { id } = req.params;
    const { name, keyword, match_type, message_type, content, embed_json } = req.body;

    try {
        const [result] = await pool.query(`
            INSERT INTO autoresponders (server_id, name, keyword, match_type, message_type, content, embed_json)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        `, [
            id, name, keyword, match_type || 'contains', message_type || 'text',
            content || null, embed_json || null
        ]);

        notifyBotCacheReload(id, 'autoresponder');
        res.json({ success: true, id: result.insertId });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.put('/guild/:id/autoresponders/:arId', async (req, res) => {
    const { id, arId } = req.params;
    const { name, keyword, match_type, message_type, content } = req.body;
    try {
        await pool.query(`
            UPDATE autoresponders
            SET name=?, keyword=?, match_type=?, message_type=?, content=?
            WHERE id=? AND server_id=?
        `, [name, keyword, match_type || 'contains', message_type || 'text', content || '', arId, id]);

        notifyBotCacheReload(id, 'autoresponder');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.delete('/guild/:id/autoresponders/:arId', async (req, res) => {
    const { id, arId } = req.params;
    try {
        await pool.query('DELETE FROM autoresponders WHERE id = ? AND server_id = ?', [arId, id]);
        notifyBotCacheReload(id, 'autoresponder');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.get('/guild/:id/autoreacts', async (req, res) => {
    const { id } = req.params;
    try {
        const [rows] = await pool.query('SELECT * FROM autoreacts WHERE server_id = ? ORDER BY id DESC', [id]);
        res.json(rows);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/autoreacts', async (req, res) => {
    const { id } = req.params;
    const { name, keyword, match_type, emojis } = req.body;

    try {
        const [result] = await pool.query(`
            INSERT INTO autoreacts (server_id, name, keyword, match_type, emojis)
            VALUES (?, ?, ?, ?, ?)
        `, [id, name, keyword, match_type || 'contains', emojis || '']);

        notifyBotCacheReload(id, 'autoreact');
        res.json({ success: true, id: result.insertId });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.put('/guild/:id/autoreacts/:areId', async (req, res) => {
    const { id, areId } = req.params;
    const { name, keyword, match_type, emojis } = req.body;
    try {
        await pool.query(`
            UPDATE autoreacts
            SET name=?, keyword=?, match_type=?, emojis=?
            WHERE id=? AND server_id=?
        `, [name, keyword, match_type || 'contains', emojis || '', areId, id]);

        notifyBotCacheReload(id, 'autoreact');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.delete('/guild/:id/autoreacts/:areId', async (req, res) => {
    const { id, areId } = req.params;
    try {
        await pool.query('DELETE FROM autoreacts WHERE id = ? AND server_id = ?', [areId, id]);
        notifyBotCacheReload(id, 'autoreact');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// -------------------------------------------------------------------
// 3. Command Aliases & Trusted Entities
// -------------------------------------------------------------------

router.get('/guild/:id/aliases', async (req, res) => {
    const { id } = req.params;
    try {
        const [serverAliases] = await pool.query('SELECT alias, target FROM server_command_aliases WHERE server_id = ?', [id]);
        res.json({ server_aliases: serverAliases });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/aliases', async (req, res) => {
    const { id } = req.params;
    const { alias, target } = req.body;

    try {
        await pool.query(`
            INSERT INTO server_command_aliases (server_id, alias, target)
            VALUES (?, ?, ?)
            ON DUPLICATE KEY UPDATE target=VALUES(target)
        `, [id, alias, target]);

        notifyBotCacheReload(id, 'aliases');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.delete('/guild/:id/aliases/:alias', async (req, res) => {
    const { id, alias } = req.params;
    try {
        await pool.query('DELETE FROM server_command_aliases WHERE server_id = ? AND alias = ?', [id, alias]);
        notifyBotCacheReload(id, 'aliases');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.get('/guild/:id/trust', async (req, res) => {
    const { id } = req.params;
    try {
        const [rows] = await pool.query('SELECT entity_type, entity_id FROM trusted_entities WHERE server_id = ?', [id]);
        res.json(rows);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/trust', async (req, res) => {
    const { id } = req.params;
    const { entity_type, entity_id } = req.body;

    try {
        await pool.query(`
            INSERT IGNORE INTO trusted_entities (server_id, entity_type, entity_id)
            VALUES (?, ?, ?)
        `, [id, entity_type || 'user', entity_id]);

        notifyBotCacheReload(id, 'trust');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.delete('/guild/:id/trust/:entity_type/:entity_id', async (req, res) => {
    const { id, entity_type, entity_id } = req.params;
    try {
        await pool.query('DELETE FROM trusted_entities WHERE server_id = ? AND entity_type = ? AND entity_id = ?', [id, entity_type, entity_id]);
        notifyBotCacheReload(id, 'trust');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// -------------------------------------------------------------------
// 4. Anti-Raid & Moderation Action Messages
// -------------------------------------------------------------------

router.get('/guild/:id/antiraid', async (req, res) => {
    const { id } = req.params;
    try {
        const [rows] = await pool.query('SELECT * FROM antiraid_configs WHERE server_id = ?', [id]);
        if (rows.length === 0) {
            return res.json({
                server_id: id,
                joins_threshold_count: 5,
                joins_threshold_seconds: 60,
                joins_action: 'none',
                spam_threshold_count: 5,
                spam_threshold_seconds: 2,
                spam_action: 'none',
                default_profile_action: 'none'
            });
        }
        res.json(rows[0]);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/antiraid', async (req, res) => {
    const { id } = req.params;
    const {
        joins_threshold_count, joins_threshold_seconds, joins_action,
        spam_threshold_count, spam_threshold_seconds, spam_action,
        default_profile_action
    } = req.body;

    try {
        await pool.query(`
            INSERT INTO antiraid_configs
            (server_id, joins_threshold_count, joins_threshold_seconds, joins_action,
             spam_threshold_count, spam_threshold_seconds, spam_action, default_profile_action)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            ON DUPLICATE KEY UPDATE
            joins_threshold_count=VALUES(joins_threshold_count), joins_threshold_seconds=VALUES(joins_threshold_seconds),
            joins_action=VALUES(joins_action), spam_threshold_count=VALUES(spam_threshold_count),
            spam_threshold_seconds=VALUES(spam_threshold_seconds), spam_action=VALUES(spam_action),
            default_profile_action=VALUES(default_profile_action)
        `, [
            id, joins_threshold_count || 5, joins_threshold_seconds || 60, joins_action || 'none',
            spam_threshold_count || 5, spam_threshold_seconds || 2, spam_action || 'none',
            default_profile_action || 'none'
        ]);

        notifyBotCacheReload(id, 'antiraid');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// -------------------------------------------------------------------
// 5. Event Logging & Prefixes
// -------------------------------------------------------------------

router.get('/guild/:id/logging', async (req, res) => {
    const { id } = req.params;
    try {
        const [channels] = await pool.query('SELECT event_type, channel_id FROM logging_configs WHERE server_id = ?', [id]);
        res.json({ channels });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/logging', async (req, res) => {
    const { id } = req.params;
    const { event_type, channel_id } = req.body;

    try {
        if (channel_id === '' || channel_id === null) {
            await pool.query('DELETE FROM logging_configs WHERE server_id = ? AND event_type = ?', [id, event_type]);
        } else {
            await pool.query(`
                INSERT INTO logging_configs (server_id, event_type, channel_id)
                VALUES (?, ?, ?)
                ON DUPLICATE KEY UPDATE channel_id=VALUES(channel_id)
            `, [id, event_type, channel_id]);
        }

        notifyBotCacheReload(id, 'logging');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.get('/guild/:id/prefixes', async (req, res) => {
    const { id } = req.params;
    try {
        const [rows] = await pool.query("SELECT target_type, target_id, prefixes, default_enabled FROM prefixes WHERE target_type='server' AND target_id = ?", [id]);
        if (rows.length === 0) {
            return res.json({ prefixes: ['.'], default_enabled: true });
        }
        let parsed = ['.'];
        try {
            parsed = JSON.parse(rows[0].prefixes);
        } catch (e) {
            if (rows[0].prefixes) {
                parsed = String(rows[0].prefixes).split(',').map(p => p.trim()).filter(Boolean);
            }
        }
        res.json({
            prefixes: Array.isArray(parsed) ? parsed : ['.'],
            default_enabled: rows[0].default_enabled === 1
        });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/prefixes', async (req, res) => {
    const { id } = req.params;
    const { prefixes, default_enabled } = req.body;

    try {
        await pool.query(`
            INSERT INTO prefixes (target_type, target_id, prefixes, default_enabled)
            VALUES ('server', ?, ?, ?)
            ON DUPLICATE KEY UPDATE prefixes=VALUES(prefixes), default_enabled=VALUES(default_enabled)
        `, [id, JSON.stringify(prefixes || ['.']), default_enabled ? 1 : 0]);

        notifyBotCacheReload(id, 'prefixes');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// -------------------------------------------------------------------
// 6. Leveling, Counting & Masquerade Configs
// -------------------------------------------------------------------

router.get('/guild/:id/leveling', async (req, res) => {
    const { id } = req.params;
    try {
        const [rows] = await pool.query('SELECT * FROM guild_leveling_config WHERE server_id = ?', [id]);
        if (rows.length === 0) {
            return res.json({
                config: {
                    server_id: id,
                    enabled: 0,
                    reward_coins: 1,
                    coins_per_message: 1,
                    min_xp_per_message: 15,
                    max_xp_per_message: 25,
                    min_message_chars: 5,
                    max_message_chars: 2000,
                    xp_cooldown_seconds: 60,
                    announcement_message: '{name} reached **Level {level}**!'
                }
            });
        }
        const config = rows[0];
        config.announcement_message = config.level_up_message;
        res.json({ config });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/leveling', async (req, res) => {
    const { id } = req.params;
    const {
        enabled,
        reward_coins,
        coins_per_message,
        min_xp_per_message,
        max_xp_per_message,
        min_message_chars,
        max_message_chars,
        xp_cooldown_seconds,
        announcement_message
    } = req.body;

    try {
        await pool.query(`
            INSERT INTO guild_leveling_config
            (server_id, enabled, reward_coins, coins_per_message, min_xp_per_message, max_xp_per_message, min_message_chars, max_message_chars, xp_cooldown_seconds, level_up_message)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON DUPLICATE KEY UPDATE
            enabled=VALUES(enabled), reward_coins=VALUES(reward_coins),
            coins_per_message=VALUES(coins_per_message), min_xp_per_message=VALUES(min_xp_per_message),
            max_xp_per_message=VALUES(max_xp_per_message), min_message_chars=VALUES(min_message_chars),
            max_message_chars=VALUES(max_message_chars), xp_cooldown_seconds=VALUES(xp_cooldown_seconds),
            level_up_message=VALUES(level_up_message)
        `, [
            id,
            enabled ? 1 : 0,
            reward_coins ? 1 : 0,
            coins_per_message !== undefined ? coins_per_message : 1,
            min_xp_per_message || 15,
            max_xp_per_message || 25,
            min_message_chars !== undefined ? min_message_chars : 5,
            max_message_chars !== undefined ? max_message_chars : 2000,
            xp_cooldown_seconds || 60,
            announcement_message || '{name} reached **Level {level}**!'
        ]);

        notifyBotCacheReload(id, 'leveling');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.get('/guild/:id/roles', async (req, res) => {
    const { id } = req.params;
    try {
        if (botToken) {
            const fetchRes = await fetch(`https://api.stoat.chat/servers/${id}`, {
                headers: { 'x-bot-token': botToken }
            });
            if (fetchRes.ok) {
                const sData = await fetchRes.json();
                const roles = sData.roles || {};
                const formatted = [];
                if (typeof roles === 'object' && !Array.isArray(roles)) {
                    for (const [rId, rObj] of Object.entries(roles)) {
                        formatted.push({
                            id: rId,
                            name: rObj.name,
                            color: rObj.colour || null
                        });
                    }
                } else if (Array.isArray(roles)) {
                    for (const r of roles) {
                        formatted.push({
                            id: r._id || r.id,
                            name: r.name,
                            color: r.colour || null
                        });
                    }
                }
                return res.json(formatted);
            }
        }
        res.json([]);
    } catch (err) {
        res.json([]);
    }
});

router.get('/guild/:id/leveling/roles', async (req, res) => {
    const { id } = req.params;
    try {
        const [rows] = await pool.query('SELECT * FROM guild_level_roles WHERE server_id = ? ORDER BY level ASC', [id]);
        res.json(rows);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/leveling/roles', async (req, res) => {
    const { id } = req.params;
    const { level, role_id, role_name, remove_previous } = req.body;
    if (!level || !role_id || !role_name) {
        return res.status(400).json({ error: 'level, role_id, and role_name are required' });
    }
    try {
        await pool.query(`
            INSERT INTO guild_level_roles (server_id, level, role_id, role_name, remove_previous)
            VALUES (?, ?, ?, ?, ?)
            ON DUPLICATE KEY UPDATE role_id=VALUES(role_id), role_name=VALUES(role_name), remove_previous=VALUES(remove_previous)
        `, [id, level, role_id, role_name, remove_previous ? 1 : 0]);
        notifyBotCacheReload(id, 'level_roles');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.delete('/guild/:id/leveling/roles/:level', async (req, res) => {
    const { id, level } = req.params;
    try {
        await pool.query('DELETE FROM guild_level_roles WHERE server_id = ? AND level = ?', [id, level]);
        notifyBotCacheReload(id, 'level_roles');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.get('/guild/:id/counting', async (req, res) => {
    const { id } = req.params;
    try {
        const [rows] = await pool.query('SELECT channel_id, current_number, fail_role_id, fail_role_duration, passive, warn_double_count FROM counting_configs WHERE server_id = ?', [id]);
        if (rows.length === 0) {
            return res.json({
                server_id: id,
                channel_id: '',
                current_number: 0,
                fail_role_id: '',
                fail_role_duration: 0,
                passive: 0,
                warn_double_count: 0
            });
        }
        res.json(rows[0]);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/counting', async (req, res) => {
    const { id } = req.params;
    const { channel_id, current_number, fail_role_id, fail_role_duration, passive, warn_double_count } = req.body;

    try {
        await pool.query(`
            INSERT INTO counting_configs (server_id, channel_id, current_number, fail_role_id, fail_role_duration, passive, warn_double_count)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            ON DUPLICATE KEY UPDATE 
            channel_id=VALUES(channel_id), 
            current_number=VALUES(current_number),
            fail_role_id=VALUES(fail_role_id),
            fail_role_duration=VALUES(fail_role_duration),
            passive=VALUES(passive),
            warn_double_count=VALUES(warn_double_count)
        `, [
            id, 
            channel_id || '', 
            current_number || 0,
            fail_role_id || '',
            fail_role_duration || 0,
            passive ? 1 : 0,
            warn_double_count ? 1 : 0
        ]);

        notifyBotCacheReload(id, 'counting');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.get('/guild/:id/masquerade', async (req, res) => {
    const { id } = req.params;
    try {
        const [rows] = await pool.query('SELECT name, avatar, colour, disabled FROM masquerade_configs WHERE server_id = ?', [id]);
        if (rows.length === 0) {
            return res.json({ server_id: id, name: '', avatar: '', colour: '', disabled: 0 });
        }
        res.json(rows[0]);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/masquerade', async (req, res) => {
    const { id } = req.params;
    const { name, avatar, colour, disabled, disable_user, disable_user_name, disable_user_avatar, disable_user_colour } = req.body;

    try {
        await pool.query(`
            INSERT INTO masquerade_configs (server_id, name, avatar, colour, disabled, disable_user, disable_user_name, disable_user_avatar, disable_user_colour)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON DUPLICATE KEY UPDATE
                name=VALUES(name), avatar=VALUES(avatar), colour=VALUES(colour),
                disabled=VALUES(disabled), disable_user=VALUES(disable_user),
                disable_user_name=VALUES(disable_user_name), disable_user_avatar=VALUES(disable_user_avatar),
                disable_user_colour=VALUES(disable_user_colour)
        `, [id, name || null, avatar || null, colour || null, disabled ? 1 : 0, disable_user ? 1 : 0, disable_user_name ? 1 : 0, disable_user_avatar ? 1 : 0, disable_user_colour ? 1 : 0]);

        notifyBotCacheReload(id, 'masquerade');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// Command Control Routes (Disabled Commands & Restrictions)
router.get('/guild/:id/commands', async (req, res) => {
    const { id } = req.params;
    try {
        const [disabled] = await pool.query("SELECT target_name FROM disabled_commands WHERE server_id = ? AND target_type = 'command'", [id]);
        const [restrictions] = await pool.query('SELECT target_name, restriction_type, entity_type, entity_id FROM command_restrictions WHERE server_id = ?', [id]);
        res.json({
            disabled: disabled.map(d => d.target_name),
            restrictions
        });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/commands/disable', async (req, res) => {
    const { id } = req.params;
    const { command_name } = req.body;
    if (!command_name) return res.status(400).json({ error: 'Command name required' });

    try {
        await pool.query("INSERT IGNORE INTO disabled_commands (server_id, target_type, target_name) VALUES (?, 'command', ?)", [id, command_name.toLowerCase().trim()]);
        notifyBotCacheReload(id, 'commands');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.delete('/guild/:id/commands/disable/:cmd', async (req, res) => {
    const { id, cmd } = req.params;
    try {
        await pool.query("DELETE FROM disabled_commands WHERE server_id = ? AND target_type = 'command' AND target_name = ?", [id, cmd]);
        notifyBotCacheReload(id, 'commands');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// Module Control Routes (Enabled/Disabled Cogs/Modules)
router.get('/guild/:id/modules', async (req, res) => {
    const { id } = req.params;
    try {
        const [disabled] = await pool.query("SELECT target_name FROM disabled_commands WHERE server_id = ? AND target_type = 'module'", [id]);
        res.json({
            disabled: disabled.map(d => d.target_name.toLowerCase())
        });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/modules/disable', async (req, res) => {
    const { id } = req.params;
    const { module_name } = req.body;
    if (!module_name) return res.status(400).json({ error: 'Module name required' });

    try {
        await pool.query("INSERT IGNORE INTO disabled_commands (server_id, target_type, target_name) VALUES (?, 'module', ?)", [id, module_name.toLowerCase().trim()]);
        notifyBotCacheReload(id, 'commands');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/modules/enable', async (req, res) => {
    const { id } = req.params;
    const { module_name } = req.body;
    if (!module_name) return res.status(400).json({ error: 'Module name required' });

    try {
        await pool.query("DELETE FROM disabled_commands WHERE server_id = ? AND target_type = 'module' AND target_name = ?", [id, module_name.toLowerCase().trim()]);
        notifyBotCacheReload(id, 'commands');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// Log Embed Customization Routes
router.get('/guild/:id/logging/embeds', async (req, res) => {
    const { id } = req.params;
    try {
        const [rows] = await pool.query('SELECT event_type, message_type, content, embed_json FROM logging_embeds WHERE server_id = ?', [id]);
        const embeds = rows.map(r => {
            let title = '', color = '#fd4d4d', description = '', media = '';
            try {
                const parsed = typeof r.embed_json === 'string' ? JSON.parse(r.embed_json) : r.embed_json;
                if (parsed) {
                    title = parsed.title || '';
                    color = parsed.color || parsed.colour || '#fd4d4d';
                    description = parsed.description || '';
                    media = parsed.media || parsed.image || parsed.icon_url || '';
                }
            } catch (e) {}
            return {
                event_name: r.event_type,
                title,
                color,
                description: description || r.content || '',
                media
            };
        });
        res.json({ embeds });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/logging/embeds', async (req, res) => {
    const { id } = req.params;
    const { event_name, title, color, description, media } = req.body;
    if (!event_name) return res.status(400).json({ error: 'event_name required' });

    try {
        const embedObj = {
            title: title || '',
            color: color || '#fd4d4d',
            description: description || '',
            media: media || ''
        };
        await pool.query(`
            INSERT INTO logging_embeds (server_id, event_type, message_type, content, embed_json)
            VALUES (?, ?, 'embed', ?, ?)
            ON DUPLICATE KEY UPDATE content=VALUES(content), embed_json=VALUES(embed_json)
        `, [id, event_name, description || '', JSON.stringify(embedObj)]);

        notifyBotCacheReload(id, 'logging');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// Jail & Action Messages Routes
router.get('/guild/:id/jail', async (req, res) => {
    const { id } = req.params;
    try {
        const [rows] = await pool.query('SELECT jail_role_id, jail_channel_id FROM jail_configs WHERE server_id = ?', [id]);
        if (rows.length === 0) {
            return res.json({ server_id: id, jail_role_id: '', jail_channel_id: '', auto_jail_on_join: 0 });
        }
        res.json({
            server_id: id,
            jail_role_id: rows[0].jail_role_id || '',
            jail_channel_id: rows[0].jail_channel_id || '',
            auto_jail_on_join: 0
        });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.post('/guild/:id/jail', async (req, res) => {
    const { id } = req.params;
    const { jail_role_id, jail_channel_id } = req.body;

    try {
        await pool.query(`
            INSERT INTO jail_configs (server_id, jail_role_id, jail_channel_id)
            VALUES (?, ?, ?)
            ON DUPLICATE KEY UPDATE jail_role_id=VALUES(jail_role_id), jail_channel_id=VALUES(jail_channel_id)
        `, [id, jail_role_id || null, jail_channel_id || null]);

        notifyBotCacheReload(id, 'jail');
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// Analytics Timeline Line Charts Routes
router.get('/guild/:id/analytics/activity-timeline', async (req, res) => {
    const { id } = req.params;
    try {
        const [rows] = await pool.query(`
            SELECT 
                DATE_FORMAT(stat_date, '%b %d') as label,
                DATE_FORMAT(stat_date, '%Y-%m-%d') as full_date,
                SUM(messages) as messages,
                SUM(commands_used) as commands
            FROM guild_user_activity_daily
            WHERE guild_id = ? AND is_bot = 0
            GROUP BY stat_date
            ORDER BY stat_date ASC
            LIMIT 14
        `, [id]);

        res.json(rows);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

router.get('/guild/:id/analytics/members-timeline', async (req, res) => {
    const { id } = req.params;
    try {
        const [rows] = await pool.query(`
            SELECT 
                DATE_FORMAT(event_date, '%b %d') as label,
                DATE_FORMAT(event_date, '%Y-%m-%d') as full_date,
                joins,
                leaves
            FROM guild_member_events_daily
            WHERE guild_id = ?
            ORDER BY event_date ASC
            LIMIT 14
        `, [id]);

        if (rows.length === 0) {
            const [actRows] = await pool.query(`
                SELECT DISTINCT 
                    DATE_FORMAT(stat_date, '%b %d') as label,
                    DATE_FORMAT(stat_date, '%Y-%m-%d') as full_date
                FROM guild_user_activity_daily
                WHERE guild_id = ?
                ORDER BY stat_date ASC
                LIMIT 14
            `, [id]);
            const fallback = actRows.map(r => ({ label: r.label, full_date: r.full_date, joins: 0, leaves: 0 }));
            return res.json(fallback);
        }

        res.json(rows);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// GET /api/public/serverlist
router.get('/public/serverlist', async (req, res) => {
    try {
        const [rows] = await pool.query(
            "SELECT sl.server_id, sl.name, sl.tags, sl.description, sl.invite_url, sl.last_bumped_at, sl.bump_count, s.icon_id, s.banner_id, " +
            "(SELECT COUNT(*) FROM server_members sm WHERE sm.server_id = sl.server_id) AS member_count " +
            "FROM server_listings sl " +
            "LEFT JOIN servers s ON s.server_id = sl.server_id " +
            "WHERE sl.status = 'approved' " +
            "ORDER BY sl.last_bumped_at DESC, member_count DESC"
        );
        res.json(rows);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// -------------------------------------------------------------------
// User Data Privacy & GDPR/CCPA Compliance Endpoints
// -------------------------------------------------------------------

function getAuthToken(req) {
    const authHeader = req.headers.authorization;
    return req.query.token || (authHeader ? authHeader.replace('Bearer ', '') : null);
}

async function safeSelect(query, params) {
    try {
        const [rows] = await pool.query(query, params);
        return rows;
    } catch (e) {
        return [];
    }
}

async function requireBotOwner(req, res, next) {
    const authHeader = req.get('authorization') || '';
    const match = authHeader.match(/^Bearer\s+(.+)$/i);
    if (!match) return res.status(401).json({ error: 'Authentication required.' });

    try {
        const [rows] = await pool.query(
            'SELECT user_id FROM dashboard_tokens WHERE token = ? AND expires_at > NOW()',
            [match[1]]
        );
        if (rows.length === 0) return res.status(401).json({ error: 'Invalid or expired dashboard token.' });
        if (!isBotOwner(rows[0].user_id)) return res.status(403).json({ error: 'This panel is restricted to the bot owner.' });

        res.locals.ownerUserId = rows[0].user_id;
        next();
    } catch (err) {
        console.error('Error verifying bot owner access:', err);
        res.status(500).json({ error: 'Failed to verify owner access.' });
    }
}

function validDatabaseId(value) {
    return typeof value === 'string' && value.length > 0 && value.length <= 64 &&
        /^[a-zA-Z0-9_-]+$/.test(value);
}

function validUserId(value) {
    return typeof value === 'string' && value.length > 0 && value.length <= 30 &&
        /^[a-zA-Z0-9_-]+$/.test(value);
}

function parseNonNegativeInteger(value, max = Number.MAX_SAFE_INTEGER) {
    if (!(typeof value === 'number' || (typeof value === 'string' && /^\d+$/.test(value)))) return null;
    const parsed = Number(value);
    return Number.isSafeInteger(parsed) && parsed >= 0 && parsed <= max ? parsed : null;
}

function getRequestBody(req) {
    return req.body && typeof req.body === 'object' && !Array.isArray(req.body) ? req.body : {};
}

router.use('/owner/economy', requireBotOwner);

router.get('/owner/economy/summary', async (req, res) => {
    try {
        const [[summaryRows], [leaders], [itemRows], [inventoryRows]] = await Promise.all([
            pool.query(`
                SELECT COUNT(*) AS user_count,
                       COALESCE(SUM(wallet), 0) AS wallet_total,
                       COALESCE(SUM(bank), 0) AS bank_total
                FROM users
            `),
            pool.query(`
                SELECT user_id, wallet, bank, (wallet + bank) AS total
                FROM users
                ORDER BY (wallet + bank) DESC
                LIMIT 10
            `),
            pool.query('SELECT COUNT(*) AS item_count FROM custom_shop_items'),
            pool.query('SELECT COUNT(*) AS inventory_count FROM user_economy_items')
        ]);
        res.json({
            summary: {
                user_count: summaryRows[0].user_count,
                wallet_total: summaryRows[0].wallet_total,
                bank_total: summaryRows[0].bank_total,
                item_count: itemRows[0].item_count,
                inventory_count: inventoryRows[0].inventory_count
            },
            leaders
        });
    } catch (err) {
        console.error('Error loading economy summary:', err);
        res.status(500).json({ error: 'Failed to load economy summary.' });
    }
});

router.get('/owner/economy/users', async (req, res) => {
    const search = String(req.query.q || '').trim();
    if (search.length > 64) return res.status(400).json({ error: 'Search must be 64 characters or fewer.' });

    try {
        const [users] = await pool.query(`
            SELECT user_id, wallet, bank, bank_limit, prestige
            FROM users
            WHERE user_id LIKE ?
            ORDER BY (wallet + bank) DESC
            LIMIT 100
        `, [`%${search}%`]);
        res.json({ users });
    } catch (err) {
        console.error('Error searching economy users:', err);
        res.status(500).json({ error: 'Failed to search economy users.' });
    }
});

router.get('/owner/economy/users/:userId/profile', async (req, res) => {
    const { userId } = req.params;
    if (!validUserId(userId)) return res.status(400).json({ error: 'Invalid user ID.' });

    try {
        const [profile, [accounts]] = await Promise.all([
            resolveUserDetails(userId),
            pool.query('SELECT wallet, bank, bank_limit, prestige FROM users WHERE user_id = ?', [userId])
        ]);
        if (!profile.resolved) return res.status(404).json({ error: 'Could not load a profile for that ID.' });
        res.json({
            user_id: userId,
            username: profile.username,
            avatar_url: profile.avatar_url,
            economy: accounts[0] || null
        });
    } catch (err) {
        console.error(`Error loading economy profile for ${userId}:`, err);
        res.status(500).json({ error: 'Failed to load user profile.' });
    }
});

router.put('/owner/economy/users/:userId', async (req, res) => {
    const { userId } = req.params;
    if (!validUserId(userId)) return res.status(400).json({ error: 'Invalid user ID.' });

    const body = getRequestBody(req);
    const wallet = parseNonNegativeInteger(body.wallet);
    const bank = parseNonNegativeInteger(body.bank);
    const bankLimit = parseNonNegativeInteger(body.bank_limit);
    const prestige = parseNonNegativeInteger(body.prestige, 2147483647);
    if ([wallet, bank, bankLimit, prestige].includes(null)) {
        return res.status(400).json({ error: 'Wallet, bank, bank limit, and prestige must be valid non-negative integers.' });
    }

    try {
        await pool.query(`
            INSERT INTO users (user_id, wallet, bank, bank_limit, prestige)
            VALUES (?, ?, ?, ?, ?)
            ON DUPLICATE KEY UPDATE wallet = VALUES(wallet), bank = VALUES(bank),
                bank_limit = VALUES(bank_limit), prestige = VALUES(prestige)
        `, [userId, wallet, bank, bankLimit, prestige]);
        res.json({ success: true });
    } catch (err) {
        console.error('Error updating economy balances:', err);
        res.status(500).json({ error: 'Failed to update user balances.' });
    }
});

router.get('/owner/economy/users/:userId/inventory', async (req, res) => {
    const { userId } = req.params;
    if (!validUserId(userId)) return res.status(400).json({ error: 'Invalid user ID.' });

    try {
        const [items] = await pool.query(`
            SELECT inventory.item_id, COALESCE(shop.name, inventory.item_id) AS name,
                   inventory.quantity, inventory.uses_left, inventory.active_until
            FROM user_economy_items AS inventory
            LEFT JOIN custom_shop_items AS shop ON shop.item_id = inventory.item_id
            WHERE inventory.user_id = ?
            ORDER BY inventory.item_id
        `, [userId]);
        res.json({ items });
    } catch (err) {
        console.error('Error loading user economy inventory:', err);
        res.status(500).json({ error: 'Failed to load user inventory.' });
    }
});

router.put('/owner/economy/users/:userId/inventory/:itemId', async (req, res) => {
    const { userId, itemId } = req.params;
    if (!validUserId(userId) || !validDatabaseId(itemId)) {
        return res.status(400).json({ error: 'Invalid user or item ID.' });
    }

    const body = getRequestBody(req);
    const quantity = parseNonNegativeInteger(body.quantity, 2147483647);
    const usesLeft = parseNonNegativeInteger(body.uses_left, 2147483647);
    const activeUntil = parseNonNegativeInteger(body.active_until);
    if ([quantity, usesLeft, activeUntil].includes(null)) {
        return res.status(400).json({ error: 'Quantity, uses, and active-until must be non-negative integers.' });
    }

    try {
        if (quantity === 0) {
            await pool.query('DELETE FROM user_economy_items WHERE user_id = ? AND item_id = ?', [userId, itemId]);
        } else {
            await pool.query(`
                INSERT INTO user_economy_items (user_id, item_id, quantity, uses_left, active_until)
                VALUES (?, ?, ?, ?, ?)
                ON DUPLICATE KEY UPDATE quantity = VALUES(quantity),
                    uses_left = VALUES(uses_left), active_until = VALUES(active_until)
            `, [userId, itemId, quantity, usesLeft, activeUntil]);
        }
        res.json({ success: true });
    } catch (err) {
        console.error('Error updating user economy inventory:', err);
        res.status(500).json({ error: 'Failed to update inventory item.' });
    }
});

router.get('/owner/economy/items', async (req, res) => {
    try {
        const [items] = await pool.query(`
            SELECT item_id, name, type, cost, COALESCE(description, '') AS description,
                   uses_default, duration_seconds
            FROM custom_shop_items
            ORDER BY name
        `);
        res.json({ items });
    } catch (err) {
        console.error('Error loading custom shop items:', err);
        res.status(500).json({ error: 'Failed to load shop items.' });
    }
});

function validateShopItem(body) {
    const itemId = typeof body.item_id === 'string' ? body.item_id.trim() : '';
    const name = typeof body.name === 'string' ? body.name.trim() : '';
    const type = typeof body.type === 'string' && body.type.trim() ? body.type.trim() : 'general';
    const cost = parseNonNegativeInteger(body.cost);
    const uses = parseNonNegativeInteger(body.uses_default, 2147483647);
    const duration = parseNonNegativeInteger(body.duration_seconds, 2147483647);
    const description = typeof body.description === 'string' ? body.description.trim() : '';

    if (!validDatabaseId(itemId)) return { error: 'Item ID may contain only letters, numbers, underscores, and hyphens.' };
    if (!name || name.length > 128) return { error: 'Item name is required and must be 128 characters or fewer.' };
    if (type.length > 64) return { error: 'Item type must be 64 characters or fewer.' };
    if (description.length > 65535) return { error: 'Description is too long.' };
    if ([cost, uses, duration].includes(null)) return { error: 'Cost, uses, and duration must be valid non-negative integers.' };

    return { item: { itemId, name, type, cost, description, uses, duration } };
}

async function saveShopItem(req, res, itemIdFromPath = null) {
    const body = getRequestBody(req);
    const validated = validateShopItem({ ...body, item_id: itemIdFromPath || body.item_id });
    if (validated.error) return res.status(400).json({ error: validated.error });

    const { item } = validated;
    try {
        await pool.query(`
            INSERT INTO custom_shop_items
                (item_id, name, type, cost, description, uses_default, duration_seconds)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            ON DUPLICATE KEY UPDATE name = VALUES(name), type = VALUES(type),
                cost = VALUES(cost), description = VALUES(description),
                uses_default = VALUES(uses_default), duration_seconds = VALUES(duration_seconds)
        `, [item.itemId, item.name, item.type, item.cost, item.description, item.uses, item.duration]);
        res.json({ success: true });
    } catch (err) {
        console.error('Error saving custom shop item:', err);
        res.status(500).json({ error: 'Failed to save shop item.' });
    }
}

router.post('/owner/economy/items', (req, res) => saveShopItem(req, res));
router.put('/owner/economy/items/:itemId', (req, res) => {
    if (!validDatabaseId(req.params.itemId)) return res.status(400).json({ error: 'Invalid item ID.' });
    return saveShopItem(req, res, req.params.itemId);
});

router.delete('/owner/economy/items/:itemId', async (req, res) => {
    if (!validDatabaseId(req.params.itemId)) return res.status(400).json({ error: 'Invalid item ID.' });
    try {
        const [result] = await pool.query('DELETE FROM custom_shop_items WHERE item_id = ?', [req.params.itemId]);
        if (result.affectedRows === 0) return res.status(404).json({ error: 'Shop item not found.' });
        res.json({ success: true });
    } catch (err) {
        console.error('Error deleting custom shop item:', err);
        res.status(500).json({ error: 'Failed to delete shop item.' });
    }
});

// GET /api/user/data/export?format=json|csv
router.get('/user/data/export', async (req, res) => {
    const token = getAuthToken(req);
    if (!token) return res.status(401).json({ error: 'Unauthorized' });

    try {
        const [tokenRows] = await pool.query(
            'SELECT user_id FROM dashboard_tokens WHERE token = ? AND expires_at > NOW()', [token]
        );
        if (tokenRows.length === 0) return res.status(401).json({ error: 'Unauthorized or session expired' });
        const userId = tokenRows[0].user_id;

        const userData = {
            export_timestamp: new Date().toISOString(),
            user_id: userId,
            economy: await safeSelect('SELECT * FROM economy_users WHERE user_id = ?', [userId]),
            economy_inventory: await safeSelect('SELECT * FROM user_economy_items WHERE user_id = ?', [userId]),
            rank_card: await safeSelect('SELECT * FROM user_rank_cards WHERE user_id = ?', [userId]),
            fishing_stats: await safeSelect('SELECT * FROM fishing_users WHERE user_id = ?', [userId]),
            caught_fish: await safeSelect('SELECT * FROM caught_fish WHERE user_id = ?', [userId]),
            fish_bait: await safeSelect('SELECT * FROM fish_bait_inventory WHERE user_id = ?', [userId]),
            leveling_xp: await safeSelect('SELECT * FROM user_xp WHERE user_id = ?', [userId]),
            geoguessr_stats: await safeSelect('SELECT * FROM geo_stats WHERE user_id = ?', [userId]),
            counting_stats: await safeSelect('SELECT * FROM counting_stats WHERE user_id = ?', [userId]),
            account_links: await safeSelect('SELECT * FROM account_links WHERE stoat_id = ? OR fluxer_id = ? OR discord_id = ?', [userId, userId, userId]),
            roblox_link: await safeSelect('SELECT * FROM roblox_links WHERE user_id = ?', [userId]),
            lastfm_link: await safeSelect('SELECT * FROM lastfm_links WHERE user_id = ?', [userId]),
            afk_status: await safeSelect('SELECT * FROM user_afk WHERE user_id = ?', [userId]),
            command_aliases: await safeSelect('SELECT * FROM user_command_aliases WHERE user_id = ?', [userId]),
            tupper_profiles: await safeSelect('SELECT * FROM tuppers WHERE user_id = ?', [userId]),
            masquerade: await safeSelect('SELECT * FROM user_masquerades WHERE user_id = ?', [userId]),
            reminders: await safeSelect('SELECT * FROM reminders WHERE user_id = ?', [userId]),
            application_submissions: await safeSelect('SELECT * FROM application_submissions WHERE user_id = ?', [userId]),
            moderation_history: await safeSelect('SELECT * FROM moderation_logs WHERE target_id = ? OR moderator_id = ?', [userId, userId])
        };

        const format = (req.query.format || 'json').toLowerCase();

        if (format === 'csv') {
            let csvLines = ['"SECTION","FIELD","VALUE"'];
            csvLines.push(`"METADATA","Export Date","${userData.export_timestamp}"`);
            csvLines.push(`"METADATA","User ID","${userData.user_id}"`);
            for (const [section, records] of Object.entries(userData)) {
                if (Array.isArray(records)) {
                    records.forEach((row, idx) => {
                        for (const [key, val] of Object.entries(row)) {
                            const cleanVal = String(val !== null && val !== undefined ? val : '').replace(/"/g, '""');
                            csvLines.push(`"${section}[${idx}]","${key}","${cleanVal}"`);
                        }
                    });
                }
            }
            res.setHeader('Content-Type', 'text/csv');
            res.setHeader('Content-Disposition', `attachment; filename="bronx_data_${userId}.csv"`);
            return res.send(csvLines.join('\n'));
        }

        res.setHeader('Content-Type', 'application/json');
        res.setHeader('Content-Disposition', `attachment; filename="bronx_data_${userId}.json"`);
        res.json(userData);
    } catch (err) {
        console.error('Error exporting user data:', err);
        res.status(500).json({ error: 'Failed to export user data' });
    }
});

// DELETE /api/user/data/delete — wipe all user data immediately
const deleteUserDataHandler = async (req, res) => {
    const token = getAuthToken(req);
    if (!token) return res.status(401).json({ error: 'Unauthorized' });

    try {
        const [tokenRows] = await pool.query(
            'SELECT user_id FROM dashboard_tokens WHERE token = ? AND expires_at > NOW()', [token]
        );
        if (tokenRows.length === 0) return res.status(401).json({ error: 'Unauthorized or session expired' });
        const userId = tokenRows[0].user_id;

        // Ensure opt-out table exists
        await pool.query(`
            CREATE TABLE IF NOT EXISTS privacy_optouts (
                user_id VARCHAR(30) NOT NULL PRIMARY KEY,
                opted_out_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
            )
        `).catch(() => {});

        const wipeQueries = [
            // Economy
            ['DELETE FROM economy_users WHERE user_id = ?', [userId]],
            ['DELETE FROM user_economy_items WHERE user_id = ?', [userId]],
            ['DELETE FROM user_rank_cards WHERE user_id = ?', [userId]],
            // Fishing
            ['DELETE FROM fishing_users WHERE user_id = ?', [userId]],
            ['DELETE FROM caught_fish WHERE user_id = ?', [userId]],
            ['DELETE FROM fish_bait_inventory WHERE user_id = ?', [userId]],
            // XP / Leveling — all servers
            ['DELETE FROM user_xp WHERE user_id = ?', [userId]],
            // Activity logs — removes from leaderboards and top-user stats
            ['DELETE FROM guild_user_activity_daily WHERE user_id = ?', [userId]],
            ['DELETE FROM user_activity WHERE user_id = ?', [userId]],
            // Command logs
            ['DELETE FROM cmd_history WHERE user_id = ?', [userId]],
            ['DELETE FROM command_history WHERE user_id = ?', [userId]],
            ['DELETE FROM guild_command_usage WHERE user_id = ?', [userId]],
            // Games
            ['DELETE FROM geo_stats WHERE user_id = ?', [userId]],
            ['DELETE FROM counting_stats WHERE user_id = ?', [userId]],
            // Account links
            ['DELETE FROM account_links WHERE stoat_id = ? OR fluxer_id = ? OR discord_id = ?', [userId, userId, userId]],
            ['DELETE FROM roblox_links WHERE user_id = ?', [userId]],
            ['DELETE FROM lastfm_links WHERE user_id = ?', [userId]],
            // Profile / customization
            ['DELETE FROM user_afk WHERE user_id = ?', [userId]],
            ['DELETE FROM user_command_aliases WHERE user_id = ?', [userId]],
            ['DELETE FROM user_masquerades WHERE user_id = ?', [userId]],
            // Tuppers
            ['DELETE FROM tuppers WHERE user_id = ?', [userId]],
            ['DELETE FROM tupper_proxies WHERE user_id = ?', [userId]],
            // Reminders
            ['DELETE FROM reminders WHERE user_id = ?', [userId]],
            // Applications & tickets
            ['DELETE FROM application_submissions WHERE user_id = ?', [userId]],
            ['DELETE FROM tickets WHERE user_id = ?', [userId]],
            // Jails
            ['DELETE FROM active_jails WHERE user_id = ?', [userId]],
            // Moderation records where they are the target
            ['DELETE FROM mod_history WHERE target_id = ?', [userId]],
            ['DELETE FROM moderation_logs WHERE target_id = ?', [userId]],
            // XP blacklist entries targeting this user
            ['DELETE FROM xp_blacklist WHERE target_type = ? AND target_id = ?', ['user', userId]],
            // Server membership records (removes from all per-server leaderboards)
            ['DELETE FROM server_members WHERE user_id = ?', [userId]],
            // Session tokens
            ['DELETE FROM dashboard_tokens WHERE user_id = ?', [userId]]
        ];

        for (const [sql, params] of wipeQueries) {
            try { await pool.query(sql, params); } catch (e) { /* table may not exist in all deployments */ }
        }

        // Mark as opted out — bot checks this table before writing new activity rows
        await pool.query(
            'INSERT INTO privacy_optouts (user_id) VALUES (?) ON DUPLICATE KEY UPDATE opted_out_at = NOW()',
            [userId]
        ).catch(() => {});

        notifyBotCacheReload('all', 'user_purge');
        res.json({
            success: true,
            message: 'your data has been removed. bronx will no longer track activity for your account.'
        });
    } catch (err) {
        console.error('Error deleting user data:', err);
        res.status(500).json({ error: 'failed to delete user data' });
    }
};

router.delete('/user/data/delete', deleteUserDataHandler);
router.post('/user/data/delete', deleteUserDataHandler);

module.exports = router;
