import { useEffect } from 'react';
import { Link, NavLink, Outlet } from 'react-router-dom';

import { useAuth } from '../auth/AuthContext';

function userInitial(emailOrName: string | null | undefined): string {
  const s = (emailOrName || '').trim();
  if (!s) return '?';
  return s.slice(0, 1).toUpperCase();
}

const NAV_ITEMS = [
  { to: '/dashboard', label: '控制台' },
  { to: '/tokens', label: 'API 令牌' },
  { to: '/models', label: '模型' },
  { to: '/topup', label: '余额' },
  { to: '/usage', label: '用量' },
];

export function AppLayout() {
  const { user, logout, loading } = useAuth();

  useEffect(() => {
    document.documentElement.classList.remove('admin-html');
    document.body.classList.remove('admin-body');
    document.documentElement.classList.add('app-html');
    document.body.classList.add('app-body');
    return () => {
      document.documentElement.classList.remove('app-html');
      document.body.classList.remove('app-body');
    };
  }, []);

  const displayEmail = user?.email || user?.username || '';
  const isRoot = user?.role === 'root';

  return (
    <div className="rlm-shell">
      <header className="rlm-topbar">
        <div className="rlm-topbar-inner">
          <Link to="/" className="rlm-brand">
            <img src="/assets/revlm_icon.svg" alt="Revlm" />
            Revlm
          </Link>

          <nav className="rlm-topnav">
            {NAV_ITEMS.map((item) => (
              <NavLink
                key={item.to}
                to={item.to}
                className={({ isActive }) => `rlm-topnav-link${isActive ? ' active' : ''}`}
              >
                {item.label}
              </NavLink>
            ))}
            {isRoot ? (
              <NavLink to="/admin" className={({ isActive }) => `rlm-topnav-link${isActive ? ' active' : ''}`}>
                管理
              </NavLink>
            ) : null}
          </nav>

          <div className="rlm-topbar-user">
            <div className="dropdown">
              <a
                href="#"
                className="d-flex align-items-center text-body text-decoration-none dropdown-toggle"
                id="dropdownUser1"
                data-bs-toggle="dropdown"
                aria-expanded="false"
                onClick={(e) => e.preventDefault()}
              >
                <span className="rlm-avatar me-2">{userInitial(displayEmail)}</span>
                <span className="d-none d-sm-inline fw-medium small text-secondary">{displayEmail || '未登录'}</span>
              </a>

              <ul
                className="dropdown-menu dropdown-menu-end border-0 shadow-lg mt-2 p-2 rounded-4"
                aria-labelledby="dropdownUser1"
              >
                <li>
                  <div className="dropdown-header">角色: {user?.role || '-'}</div>
                </li>
                <li>
                  <hr className="dropdown-divider" />
                </li>
                <li>
                  <Link className="dropdown-item rounded-2" to="/account">
                    <span className="me-2 material-symbols-rounded">manage_accounts</span>账号设置
                  </Link>
                </li>
                <li>
                  <hr className="dropdown-divider" />
                </li>
                <li>
                  <button
                    className="dropdown-item rounded-2 text-danger"
                    type="button"
                    disabled={loading}
                    onClick={() => void logout()}
                  >
                    <span className="me-2 material-symbols-rounded">logout</span>退出登录
                  </button>
                </li>
              </ul>
            </div>
          </div>
        </div>
      </header>

      <main className="content-scrollable">
        <div className="rlm-container">
          <Outlet />
        </div>
      </main>
    </div>
  );
}
