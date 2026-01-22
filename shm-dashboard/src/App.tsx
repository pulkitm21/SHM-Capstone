import './App.css';
import Navbar from './components/Navbar/Navbar';
import Topbar from './components/TopBar/TopBar';
import Tiles from './components/Tiles/Tiles';

function App() {
  const tiles = [
    {
      id: '1',
      title: 'Template Tile',
      count: 1,
      color: '#e3f2fd',
      icon: '🧩',
    },
  ];

  return (
    <div className="app-shell">
      <Navbar />
      <div className="main">
        <Topbar />
        <Tiles tiles={tiles} />
      </div>
    </div>
  );
}

export default App;
