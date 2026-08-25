import axios from 'axios';

const BASE = import.meta.env.VITE_NERVFS_BRIDGE_URL || '/api';
const TOKEN_KEY = 'nervfsAdminToken';

const api = axios.create({ baseURL: BASE });

api.interceptors.request.use((config) => {
  const token = localStorage.getItem(TOKEN_KEY);
  if (token) {
    config.headers.Authorization = `Bearer ${token}`;
  }
  return config;
});

export const getToken = () => localStorage.getItem(TOKEN_KEY);

export const login = async (password) => {
  const response = await api.post('/auth/login', { password });
  localStorage.setItem(TOKEN_KEY, response.data.token);
};

export const logout = () => {
  localStorage.removeItem(TOKEN_KEY);
};

const withTiming = async (requestPromise) => {
  const start = performance.now();
  const response = await requestPromise;
  const clientMs = performance.now() - start;
  const serverMs = typeof response.data?.serverMs === 'number' ? response.data.serverMs : null;
  return { data: response.data, serverMs, clientMs };
};

export const listFiles = () => withTiming(api.get('/files'));

export const uploadFile = (file, onProgress) => {
  const form = new FormData();
  form.append('file', file);

  return withTiming(
    api.post('/files', form, {
      onUploadProgress: (event) => {
        if (onProgress && event.total) {
          onProgress(Math.round((event.loaded * 100) / event.total));
        }
      },
    })
  );
};

export const downloadFile = (name) => {
  const token = getToken();
  const query = token ? `?token=${encodeURIComponent(token)}` : '';
  window.location.href = `${BASE}/files/${encodeURIComponent(name)}${query}`;
};

export const deleteFile = (name) => withTiming(api.delete(`/files/${encodeURIComponent(name)}`));

export const setPermissions = (name, mode) =>
  withTiming(api.patch(`/files/${encodeURIComponent(name)}/permissions`, { mode }));