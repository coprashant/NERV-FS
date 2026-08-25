import React, { useCallback, useEffect, useState } from 'react';
import { getToken, listFiles, login, logout } from './api/nervfsApi';
import FileList from './components/FileList';
import UploadForm from './components/UploadForm';
import Toast from './components/Toast';

export default function App() {
  const [authenticated, setAuthenticated] = useState(Boolean(getToken()));
  const [password, setPassword] = useState('');
  const [files, setFiles] = useState([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [loginError, setLoginError] = useState('');
  const [loggingIn, setLoggingIn] = useState(false);
  const [toast, setToast] = useState(null);

  const showToast = useCallback((type, title, message, serverMs = null, clientMs = null) => {
    setToast({ type, title, message, serverMs, clientMs });
  }, []);

  const refreshFiles = useCallback(async () => {
    if (!authenticated) {
      setLoading(false);
      return;
    }

    try {
      setError('');
      const response = await listFiles();
      setFiles(response.data?.files || []);
      
      if (response.serverMs !== null || response.clientMs !== null) {
        showToast('info', 'Files Refreshed', 'File list updated successfully.', response.serverMs, response.clientMs);
      }
    } catch (err) {
      if (err.response?.status === 401) {
        logout();
        setAuthenticated(false);
        setFiles([]);
        setLoginError('Please log in again.');
        return;
      }

      const errMsg = err.response?.data?.error || err.message || 'Could not load files.';
      setError(errMsg);
      showToast('error', 'Error', errMsg);
    } finally {
      setLoading(false);
    }
  }, [authenticated, showToast]);

  useEffect(() => {
    refreshFiles();
  }, [refreshFiles]);

  async function handleLogin(event) {
    event.preventDefault();
    setLoggingIn(true);
    setLoginError('');

    try {
      await login(password);
      setPassword('');
      setAuthenticated(true);
      setLoading(true);
    } catch (err) {
      const errMsg = err.response?.data?.error || err.message || 'Login failed.';
      setLoginError(errMsg);
      showToast('error', 'Login Failed', errMsg);
    } finally {
      setLoggingIn(false);
    }
  }

  function handleLogout() {
    logout();
    setAuthenticated(false);
    setFiles([]);
    setError('');
    setToast(null);
  }

  if (!authenticated) {
    return (
      <main className="appShell loginShell">
        <form className="loginPanel" onSubmit={handleLogin}>
          <h1>NERV-FS</h1>
          <p className="muted">Admin login</p>
          <label>
            Password
            <input
              type="password"
              value={password}
              onChange={(event) => setPassword(event.target.value)}
              autoComplete="current-password"
              autoFocus
            />
          </label>
          {loginError && <p className="error">{loginError}</p>}
          <button type="submit" disabled={loggingIn || !password}>
            {loggingIn ? 'Logging in...' : 'Log in'}
          </button>
        </form>
        <Toast toast={toast} onClose={() => setToast(null)} />
      </main>
    );
  }

  return (
    <main className="appShell">
      <header>
        <div>
          <h1>NERV-FS</h1>
          <p>Remote file store</p>
        </div>
        <div className="headerActions">
          <button type="button" onClick={refreshFiles} disabled={loading}>
            Refresh
          </button>
          <button type="button" onClick={handleLogout}>
            Log out
          </button>
        </div>
      </header>

      <UploadForm onUploaded={refreshFiles} onNotify={showToast} />
      {error && <p className="error">{error}</p>}
      {loading ? (
        <p className="muted">Loading files...</p>
      ) : (
        <FileList files={files} onChanged={refreshFiles} onNotify={showToast} />
      )}
      
      <Toast toast={toast} onClose={() => setToast(null)} />
    </main>
  );
}