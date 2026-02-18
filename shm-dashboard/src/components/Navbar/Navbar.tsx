import { NavLink } from "react-router-dom";
import "./Navbar.css";

interface NavItem {
  id: string;
  label: string;
  icon: string;
  to: string;
}

const navItems: NavItem[] = [
  { id: "home", label: "Home", icon: "📈", to: "/" },
  { id: "sensor-control", label: "Sensor Control", icon: "📈", to: "/sensorcontrol" },
  { id: "export", label: "Export", icon: "📈", to: "/export" },
];

const Navbar = () => {
  return (
    <nav className="navbar">
      <div className="navbar-title-block">
        <h1>Cape Scott, BC</h1>
      </div>

      <ul className="navbar-menu">
        {navItems.map((item) => (
          <li key={item.id}>
            <NavLink
              to={item.to}
              end={item.to === "/"}
              className={({ isActive }) =>
                `navbar-item ${isActive ? "active" : ""}`
              }
            >
              <span className="navbar-icon">{item.icon}</span>
              <span className="navbar-label">{item.label}</span>
            </NavLink>
          </li>
        ))}
      </ul>
    </nav>
  );
};

export default Navbar;
