import './Tiles.css';

interface tile {
  id: string;
  title: string;
  count: number;
  color: string;
  icon: string;
}

interface tilesProps {
  tiles: tile[];
}

const tiles = ({ tiles }: tilesProps) => {
  return (
    <div className="tiles-container">
      {tiles.map((tile) => (
        <div key={tile.id} className="tile-card">
          <div className="tile-icon" style={{ background: tile.color }}>
            {tile.icon}
          </div>
          <div className="tile-info">
            <h3 className="tile-title">{tile.title}</h3>
            <p className="tile-count">{tile.count}</p>
          </div>
        </div>
      ))}
    </div>
  );
};

export default tiles;