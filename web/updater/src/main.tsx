import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './app.tsx';
import './index.css';

/** Follows the system's light or dark setting: the cubeui tokens switch on a `.dark` class. */
const darkQuery = window.matchMedia('(prefers-color-scheme: dark)');
const applyTheme = () => document.documentElement.classList.toggle('dark', darkQuery.matches);
applyTheme();
darkQuery.addEventListener('change', applyTheme);

const root = document.getElementById('root');
if (root === null) {
  throw new Error('index.html has no #root element');
}
createRoot(root).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
