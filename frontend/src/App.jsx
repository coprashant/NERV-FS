import React, { useCallback, useEffect, useState } from 'react';
import { getToken, listFiles, login, logout } from './api/nervfsApi';
import FileList from './components/FileList';
import UploadForm from './components/UploadForm';

export default function App() {
  const [authenticated, setAuthenticated] = useState(Boolean(getToken()));
  const [password, setPassword] = useState('');
  const [files, setFiles] = useState([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [loginError, setLoginError] = useState('');
  const [loggingIn, setLoggingIn] = useState(false);

  const refreshFiles = useCallback(async () => {
    if (!authenticated) {
      setLoading(false);
      return;
    }

    try {
      setError('');
      const data = await listFiles();
      setFiles(data.files || []);
    } catch (err) {
      if (err.response?.status === 401) {
        logout();
        setAuthenticated(false);
        setFiles([]);
        setLoginError('Please log in again.');
        return;
      }

      setError(err.response?.data?.error || err.message || 'Could not load files.');
    } finally {
      setLoading(false);
    }
  }, [authenticated]);

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
      setLoginError(err.response?.data?.error || err.message || 'Login failed.');
    } finally {
      setLoggingIn(false);
    }
  }

  function handleLogout() {
    logout();
    setAuthenticated(false);
    setFiles([]);
    setError('');
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

      <UploadForm onUploaded={refreshFiles} />
      {error && <p className="error">{error}</p>}
      {loading ? <p className="muted">Loading files...</p> : <FileList files={files} onChanged={refreshFiles} />}
    </main>
  );
}
