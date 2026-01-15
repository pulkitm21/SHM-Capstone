import { useState } from 'react';
import './Navbar.css';

interface NavItem {
  id: string;
  label: string;
  icon: string;
}

const Navbar = () => {
  const [activeItem, setActiveItem] = useState('home');

  const navItems: NavItem[] = [
    { id: 'home', label: 'Home', icon: '' },
    { id: 'sensor control', label: 'Sensor Control', icon: '' },
    { id: 'system analytics', label: 'System Analytics', icon: '' },
    { id: 'export', label: 'Export', icon: '' },
    { id: 'user management', label: 'User Management', icon: '' },
    { id: 'settings', label: 'Settings', icon: '' },
  ];

  return (
    <nav className="navbar">
      <div className="navbar-brand">
        <h2>Cape Scott</h2>
      </div>
      <ul className="navbar-menu">
        {navItems.map((item) => (
          <li
            key={item.id}
            className={`navbar-item ${activeItem === item.id ? 'active' : ''}`}
            onClick={() => setActiveItem(item.id)}
          >
            <span className="navbar-icon">{item.icon}</span>
            <span className="navbar-label">{item.label}</span>
          </li>
        ))}
      </ul>
    </nav>
  );
};

export default Navbar;