import React, { useEffect } from 'react';

export const Toast = ({ toast, onClose }) => {
  if (!toast) return null;

  const { title, message, type = 'info', serverMs, clientMs, metrics, duration = 6000 } = toast;

  useEffect(() => {
    if (duration > 0) {
      const timer = setTimeout(() => {
        onClose();
      }, duration);
      return () => clearTimeout(timer);
    }
  }, [toast, duration, onClose]);

  /* Early 2000s icons */
  const getIcon = () => {
    switch (type) {
      case 'error':
        return '❌';
      case 'success':
        return '🗹';
      case 'info':
      default:
        return '🛈';
    }
  };

  return (
    <div style={styles.toastContainer}>
      <div style={styles.toastWindow}>
        <div style={styles.titleBar}>
          <span style={styles.titleText}>
            {type === 'error' ? 'System Warning' : 'System Message'}
          </span>
          <button style={styles.closeIconButton} onClick={onClose} aria-label="Close">
            ✕
          </button>
        </div>
        <div style={styles.toastBody}>
          <div style={styles.contentRow}>
            <span style={styles.icon}>{getIcon()}</span>
            <div style={styles.textContent}>
              {title && <strong style={styles.heading}>{title}</strong>}
              <p style={styles.message}>{message}</p>
            </div>
          </div>

          {/* Detailed latency metrics */}
          <div style={styles.metricsPanel}>
            {metrics ? (
              <>
                <div style={styles.metricRow}>
                  <span>Net Read:</span>
                  <strong>{Math.round(metrics.networkMs || 0)}ms</strong>
                </div>
                <div style={styles.metricRow}>
                  <span>RAM Write:</span>
                  <strong>{Math.round(metrics.writeMs || 0)}ms</strong>
                </div>
                <div style={styles.metricRow}>
                  <span>Disk Sync:</span>
                  <strong>{Math.round(metrics.syncMs || 0)}ms</strong>
                </div>
              </>
            ) : (
              <>
                {serverMs !== null && serverMs !== undefined && (
                  <div style={styles.metricRow}>
                    <span>Server:</span>
                    <strong>{Math.round(serverMs)}ms</strong>
                  </div>
                )}
                {clientMs !== null && clientMs !== undefined && (
                  <div style={styles.metricRow}>
                    <span>Client:</span>
                    <strong>{Math.round(clientMs)}ms</strong>
                  </div>
                )}
              </>
            )}
          </div>

          <div style={styles.actionRow}>
            <button style={styles.okButton} onClick={onClose}>
              OK
            </button>
          </div>
        </div>
      </div>
    </div>
  );
};

const styles = {
  toastContainer: {
    position: 'fixed',
    bottom: '20px',
    right: '20px',
    zIndex: 9999,
  },
  toastWindow: {
    width: '320px',
    border: '1px solid #0a246a',
    borderRadius: '6px 6px 0 0',
    background: '#ece9d8',
    boxShadow: '4px 4px 14px rgba(0, 0, 0, 0.45)',
    overflow: 'hidden',
    fontFamily: 'Tahoma, Verdana, "MS Sans Serif", Arial, sans-serif',
    fontSize: '12px',
  },
  titleBar: {
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'space-between',
    padding: '4px 6px 4px 8px',
    background: 'linear-gradient(180deg, #1c5cd1 0%, #0a246a 45%, #163a8f 55%, #3a86ea 100%)',
    color: '#ffffff',
  },
  titleText: {
    fontWeight: 'bold',
    fontSize: '12px',
    letterSpacing: '0.3px',
  },
  closeIconButton: {
    width: '18px',
    height: '18px',
    minHeight: '18px',
    padding: 0,
    lineHeight: '1',
    fontWeight: 'bold',
    fontSize: '10px',
    color: '#ffffff',
    background: 'linear-gradient(180deg, #ff6a5e 0%, #c00000 100%)',
    borderColor: '#7a0000',
    borderStyle: 'solid',
    borderWidth: '1px',
    borderRadius: '3px',
    cursor: 'pointer',
  },
  toastBody: {
    padding: '12px',
    display: 'flex',
    flexDirection: 'column',
    gap: '10px',
  },
  contentRow: {
    display: 'flex',
    alignItems: 'flex-start',
    gap: '10px',
  },
  icon: {
    fontSize: '20px',
    lineHeight: 1,
  },
  textContent: {
    display: 'flex',
    flexDirection: 'column',
    gap: '2px',
  },
  heading: {
    color: '#0a246a',
  },
  message: {
    margin: 0,
    color: '#000000',
    wordBreak: 'break-word',
  },
  metricsPanel: {
    display: 'flex',
    flexDirection: 'column',
    gap: '2px',
    background: '#ffffff',
    border: '1px solid #716f64',
    padding: '6px 8px',
    fontSize: '11px',
    color: '#454545',
    boxShadow: 'inset 1px 1px 2px rgba(0, 0, 0, 0.2)',
  },
  metricRow: {
    display: 'flex',
    justifyContent: 'space-between',
    alignItems: 'center',
  },
  actionRow: {
    display: 'flex',
    justifyContent: 'flex-end',
  },
  okButton: {
    minHeight: '24px',
    padding: '2px 14px',
    fontSize: '11px',
  },
};

export default Toast;