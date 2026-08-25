import React, { useCallback, useEffect, useState } from "react";
import { getToken, listFiles, login, logout } from "./api/nervfsApi";
import FileList from "./components/FileList";
import UploadForm from "./components/UploadForm";
import Toast from "./components/Toast";

function LockIcon() {
  return (
    <svg
      className="loginIcon"
      viewBox="0 0 24 24"
      aria-hidden="true"
      focusable="false"
    >
      <path
        d="M7 10V7a5 5 0 0 1 10 0v3"
        fill="none"
        stroke="currentColor"
        strokeWidth="2"
      />
      <rect x="5" y="10" width="14" height="11" rx="1" fill="currentColor" />
      <circle cx="12" cy="15" r="1.4" fill="#ece9d8" />
      <path d="M12 16.5v2" stroke="#ece9d8" strokeWidth="1.4" />
    </svg>
  );
}

function ComputerIcon() {
  return (
    <svg
      className="computerIcon"
      viewBox="0 0 32 32"
      aria-hidden="true"
      focusable="false"
    >
      <rect
        x="4"
        y="4"
        width="24"
        height="18"
        rx="1"
        fill="#d4d0c8"
        stroke="#404040"
        strokeWidth="1.5"
      />
      <rect x="7" y="7" width="18" height="12" fill="#0a246a" />
      <path
        d="M12 27h8M16 22v5"
        stroke="#404040"
        strokeWidth="2"
        strokeLinecap="square"
      />
    </svg>
  );
}

export default function App() {
  const [authenticated, setAuthenticated] = useState(Boolean(getToken()));
  const [password, setPassword] = useState("");
  const [files, setFiles] = useState([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [loginError, setLoginError] = useState("");
  const [loggingIn, setLoggingIn] = useState(false);
  const [toast, setToast] = useState(null);

  const showToast = useCallback(
    (type, title, message, serverMs = null, clientMs = null) => {
      setToast({ type, title, message, serverMs, clientMs });
    },
    [],
  );

  const refreshFiles = useCallback(async () => {
    if (!authenticated) {
      setLoading(false);
      return;
    }

    try {
      setError("");
      const response = await listFiles();
      setFiles(response.data?.files || []);

      if (response.serverMs !== null || response.clientMs !== null) {
        showToast(
          "info",
          "Files Refreshed",
          "File list updated successfully.",
          response.serverMs,
          response.clientMs,
        );
      }
    } catch (err) {
      if (err.response?.status === 401) {
        logout();
        setAuthenticated(false);
        setFiles([]);
        setLoginError("Please log in again.");
        return;
      }

      const errMsg =
        err.response?.data?.error || err.message || "Could not load files.";

      setError(errMsg);
      showToast("error", "Error", errMsg);
    } finally {
      setLoading(false);
    }
  }, [authenticated, showToast]);

  useEffect(() => {
    refreshFiles();
  }, [refreshFiles]);

  async function handleLogin(event) {
    event.preventDefault();

    if (!password || loggingIn) {
      return;
    }

    setLoggingIn(true);
    setLoginError("");

    try {
      await login(password);
      setPassword("");
      setAuthenticated(true);
      setLoading(true);
    } catch (err) {
      const errMsg =
        err.response?.data?.error || err.message || "Login failed.";

      setLoginError(errMsg);
      showToast("error", "Login Failed", errMsg);
    } finally {
      setLoggingIn(false);
    }
  }

  function handleLogout() {
    logout();
    setAuthenticated(false);
    setFiles([]);
    setError("");
    setToast(null);
  }

  if (!authenticated) {
    return (
      <main className="appShell loginShell">
        <form className="loginPanel" onSubmit={handleLogin}>
          <div className="loginTitleBar">
            <div className="loginTitle">
              <LockIcon />
              <span>NERV-FS Login</span>
            </div>

            <span className="loginWindowButton">×</span>
          </div>

          <div className="loginContent">
            <div className="loginIdentity">
              <div className="computerFrame">
                <img className="loginLogo" src="/favicon.ico" alt="NERV-FS" />
              </div>

              <div>
                <h1>NERV-FS</h1>
                <p>Remote File Store</p>
              </div>
            </div>

            <div className="loginDivider" />

            <p className="loginInstruction">
              Enter your password to access the remote file store.
            </p>

            <label className="loginPassword">
              Password
              <input
                type="password"
                value={password}
                onChange={(event) => {
                  setPassword(event.target.value);
                  if (loginError) {
                    setLoginError("");
                  }
                }}
                autoComplete="current-password"
                autoFocus
                disabled={loggingIn}
                aria-invalid={Boolean(loginError)}
              />
            </label>

            {loginError && (
              <p className="loginError" role="alert">
                <svg viewBox="0 0 20 20" aria-hidden="true" focusable="false">
                  <path d="M10 2 18 17H2L10 2Z" fill="currentColor" />
                  <path
                    d="M10 7v5"
                    stroke="#fff"
                    strokeWidth="1.8"
                    strokeLinecap="square"
                  />
                  <rect x="9.15" y="14" width="1.7" height="1.7" fill="#fff" />
                </svg>

                <span>{loginError}</span>
              </p>
            )}

            <div className="loginActions">
              <button
                type="submit"
                className="loginPrimary"
                disabled={loggingIn || !password}
              >
                {loggingIn ? "Logging in..." : "Log in"}
              </button>

              <button
                type="button"
                onClick={() => {
                  setPassword("");
                  setLoginError("");
                }}
                disabled={loggingIn || !password}
              >
                Clear
              </button>
            </div>
          </div>

          <div className="loginStatusBar">
            <span>
              <span className="statusLight" />
              Secure connection
            </span>
            <span>NERV-FS</span>
          </div>
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
