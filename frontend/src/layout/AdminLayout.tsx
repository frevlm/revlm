import { useEffect, useMemo } from 'react';
import { Link, NavLink, Outlet } from 'react-router-dom';

import { useAuth } from '../auth/AuthContext';

function userEmail(userEmailValue: string | null | undefined, username: string | null | undefined): string {
  const email = (userEmailValue || '').trim();
  if (email) return email;
  const u = (username || '').trim();
  if (u) return u;
  return '未登录';
}

const NAV_ITEMS = [
  { to: '/admin/dashboard', label: '仪表盘' },
  { to: '/admin/channels', label: '上游渠道' },
  { to: '/admin/channel-groups', label: '渠道组' },
  { to: '/admin/users', label: '用户' },
  { to: '/admin/usage', label: '用量' },
];

export function AdminLayout() {
  const { user } = useAuth();

  useEffect(() => {
    document.documentElement.classList.remove('app-html');
    document.body.classList.remove('app-body');
    document.documentElement.classList.add('admin-html');
    document.body.classList.add('admin-body');
    return () => {
      document.documentElement.classList.remove('admin-html');
      document.body.classList.remove('admin-body');
    };
  }, []);

  const loginLabel = useMemo(() => userEmail(user?.email, user?.username), [user?.email, user?.username]);

  return (
    <div className="rlm-shell rlm-shell-admin">
      <header className="rlm-topbar">
        <div className="rlm-topbar-inner">
          <Link to="/admin/dashboard" className="rlm-brand">
            <img src="/assets/revlm_icon.svg" alt="Revlm" />
            Revlm <span className="text-secondary fw-normal">管理</span>
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
          </nav>

          <div className="rlm-topbar-user">
            <Link to="/dashboard" className="rlm-topnav-link">
              返回控制台
            </Link>
            <span className="fw-medium small text-secondary d-none d-sm-inline">{loginLabel}</span>
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
