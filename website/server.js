// bronx website server with REST API & Dashboard support
const express = require('express');
const path = require('path');
const cors = require('cors');
const cookieParser = require('cookie-parser');
const apiRoutes = require('./api_routes');

const app = express();
const PORT = process.env.PORT || 3008;

// Middleware
app.use(cors());
app.use(express.json());
app.use(express.urlencoded({ extended: true }));
app.use(cookieParser());

// CSP Headers Middleware
app.use((req, res, next) => {
    res.setHeader(
        'Content-Security-Policy',
        "default-src 'self' https://bronx.siqnole.dev; " +
        "script-src 'self' 'unsafe-inline' 'unsafe-eval' https://bronx.siqnole.dev https://cdn.jsdelivr.net https://cdnjs.cloudflare.com; " +
        "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com https://cdnjs.cloudflare.com; " +
        "img-src 'self' data: https:; " +
        "font-src 'self' https://fonts.gstatic.com https://cdnjs.cloudflare.com; " +
        "connect-src 'self' https://bronx.siqnole.dev;"
    );
    next();
});

// API Routes
app.use('/api', apiRoutes);

// Explicit Page Routes (evaluated BEFORE express.static)
app.get('/dashboard', (req, res) => {
    res.sendFile(path.join(__dirname, 'dashboard.html'));
});

app.get('/economy', (req, res) => {
    res.sendFile(path.join(__dirname, 'economy.html'));
});

app.get('/servers', (req, res) => {
    res.sendFile(path.join(__dirname, 'servers.html'));
});

app.get('/docs', (req, res) => {
    res.sendFile(path.join(__dirname, 'docs', 'index.html'));
});

app.get('/scripting', (req, res) => {
    res.redirect('/docs#custom-commands');
});

app.get('/katex', (req, res) => {
    res.sendFile(path.join(__dirname, 'katex.html'));
});

app.get('/latex', (req, res) => {
    res.sendFile(path.join(__dirname, 'katex.html'));
});

app.get('/privacy', (req, res) => {
    res.sendFile(path.join(__dirname, 'privacy.html'));
});

app.get('/', (req, res) => {
    res.sendFile(path.join(__dirname, 'index.html'));
});

// Static Files
app.use(express.static(__dirname, {
    maxAge: '1d',
    setHeaders: (res, filePath) => {
        if (filePath.endsWith('.html') || filePath.endsWith('.js') || filePath.endsWith('.css')) {
            res.setHeader('Cache-Control', 'no-cache');
        }
    }
}));

// SPA Fallback for unknown routes
app.use((req, res) => {
    res.status(404).sendFile(path.join(__dirname, 'index.html'));
});

app.listen(PORT, () => {
    console.log(`bronx website & dashboard running at http://localhost:${PORT}`);
});
