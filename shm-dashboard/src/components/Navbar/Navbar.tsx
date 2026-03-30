import { useEffect, useMemo, useState } from "react";
import { NavLink } from "react-router-dom";
import useAuth from "../../Auth/useAuth";
import { getSiteName, putSiteName } from "../../services/api";
import "./Navbar.css";

interface NavItem {
  id: string;
  label: string;
  icon: string;
  to: string;
  adminOnly?: boolean;
}

const navItems: NavItem[] = [
  { id: "home", label: "Home", icon: "🏠", to: "/" },
  { id: "sensor-management", label: "Sensor Management", icon: "🛠️", to: "/sensor-management" },
  { id: "server-management", label: "Server Management", icon: "🖥️", to: "/server-management" },
  { id: "fault-log", label: "Fault Log", icon: "⚠️", to: "/fault-log" },
  { id: "export", label: "Export", icon: "💾", to: "/export" },
  { id: "decoder", label: "Decoder", icon: "🧩", to: "/decoder" },
  /*
    The Users page will be added later. The nav item is defined now so the
    navbar structure is ready once the route and page are created.
  */
  { id: "users", label: "Users", icon: "👥", to: "/users", adminOnly: true },
];

const DEFAULT_DASHBOARD_TITLE = "Cape Scott, BC";

const Navbar = () => {
  const { user, isAdmin, logout } = useAuth();

  // Stores the shared dashboard title loaded from the backend.
  const [dashboardTitle, setDashboardTitle] = useState(DEFAULT_DASHBOARD_TITLE);

  // Controls whether the title is currently being edited.
  const [isEditingTitle, setIsEditingTitle] = useState(false);

  // Temporary input value while editing.
  const [titleInput, setTitleInput] = useState(DEFAULT_DASHBOARD_TITLE);

  // Controls the initial loading fallback for the site name.
  const [loadingTitle, setLoadingTitle] = useState(true);

  // Prevents duplicate saves when Enter and blur happen close together.
  const [isSavingTitle, setIsSavingTitle] = useState(false);

  /*
    Admin-only items are filtered from the nav for viewer users.
    This keeps the Users page hidden until the user has the correct role.
  */
  const visibleNavItems = useMemo(
    () => navItems.filter((item) => !item.adminOnly || isAdmin),
    [isAdmin]
  );

  // Load the shared site name from the backend when the navbar first mounts.
  useEffect(() => {
    const controller = new AbortController();

    async function loadSiteName() {
      try {
        const response = await getSiteName(controller.signal);
        const nextTitle = response.site_name?.trim() || DEFAULT_DASHBOARD_TITLE;
        setDashboardTitle(nextTitle);
        setTitleInput(nextTitle);
      } catch (error: any) {
  if (error?.name === "AbortError") {
    return;
  }

  console.warn("Failed to load site name from backend, using default.", error);
  setDashboardTitle(DEFAULT_DASHBOARD_TITLE);
  setTitleInput(DEFAULT_DASHBOARD_TITLE);
} finally {
  if (!controller.signal.aborted) {
    setLoadingTitle(false);
  }
}
    }

    void loadSiteName();

    return () => controller.abort();
  }, []);

  async function handleLogout() {
    try {
      /*
        Logout is delegated to the shared auth context so navbar does not
        manage auth state directly.
      */
      await logout();
    } catch (error) {
      console.error("Failed to log out cleanly.", error);
    }
  }

  // Opens title edit mode using the latest saved title.
  function handleStartEditingTitle() {
    if (loadingTitle || !isAdmin) return;
    setTitleInput(dashboardTitle);
    setIsEditingTitle(true);
  }

  // Saves the dashboard title to the backend so all dashboards reflect the same value.
  async function handleSaveTitle() {
    if (isSavingTitle || !isAdmin) return;

    const trimmed = titleInput.trim();
    const nextTitle = trimmed || DEFAULT_DASHBOARD_TITLE;

    try {
      setIsSavingTitle(true);
      const response = await putSiteName({ site_name: nextTitle });
      const savedTitle = response.site_name?.trim() || DEFAULT_DASHBOARD_TITLE;

      setDashboardTitle(savedTitle);
      setTitleInput(savedTitle);
    } catch (error) {
      console.error("Failed to update site name.", error);
      setTitleInput(dashboardTitle);
    } finally {
      setIsSavingTitle(false);
      setIsEditingTitle(false);
    }
  }

  // Cancels editing and restores the currently saved title.
  function handleCancelEditingTitle() {
    setTitleInput(dashboardTitle);
    setIsEditingTitle(false);
  }

  // Saves with Enter and cancels with Escape.
  function handleTitleKeyDown(event: React.KeyboardEvent<HTMLInputElement>) {
    if (event.key === "Enter") {
      event.preventDefault();
      void handleSaveTitle();
    }

    if (event.key === "Escape") {
      handleCancelEditingTitle();
    }
  }

  return (
    <nav className="navbar">
      <div className="navbar-header">
        <div className="navbar-header-top">
          <span className="navbar-kicker">Site</span>
        </div>

        {isEditingTitle ? (
          <div className="navbar-title-editor">
            <input
              type="text"
              value={titleInput}
              onChange={(event) => setTitleInput(event.target.value)}
              onKeyDown={handleTitleKeyDown}
              onBlur={() => void handleSaveTitle()}
              className="navbar-title-input"
              maxLength={60}
              autoFocus
            />
          </div>
        ) : (
          <div className="navbar-title-row">
            <h1 className="navbar-title">
              {loadingTitle ? "Loading..." : dashboardTitle}
            </h1>

            {isAdmin && (
              <button
                type="button"
                className="navbar-edit-button"
                onClick={handleStartEditingTitle}
                aria-label="Edit dashboard title"
                title="Edit title"
                disabled={loadingTitle}
              >
                ✎
              </button>
            )}
          </div>
        )}
      </div>

      <ul className="navbar-menu">
        {visibleNavItems.map((item) => (
          <li key={item.id}>
            <NavLink
              to={item.to}
              end={item.to === "/"}
              className={({ isActive }) =>
                `navbar-item ${isActive ? "active" : ""}`
              }
            >
              <span className="navbar-icon-wrap">
                <span className="navbar-icon">{item.icon}</span>
              </span>

              <span className="navbar-label">{item.label}</span>
            </NavLink>
          </li>
        ))}
      </ul>

      <div className="navbar-footer">
        <button className="logout-button" onClick={() => void handleLogout()}>
          Logout
        </button>
      </div>
    </nav>
  );
};

export default Navbar;