import React from 'react';
import clsx from 'clsx';
import styles from './styles.module.css';

const FeatureList = [
  {
    title: 'Carefully-selected C++ libraries',
    Svg: require('@site/static/img/homepage/connection.svg').default,
    description: (
      <>
        Help connect user apps and the OS.
      </>
    ),
  },  
  {
    title: 'High performance coroutine runtime',
    Svg: require('@site/static/img/homepage/runtime.svg').default,
    description: (
      <>
        Stackful coroutine. Symmetric scheduler. Non-blocking IO engine. Support io_uring.
      </>
    ),
  },
  {
    title: 'Multiple platforms and architectures',
    Svg: require('@site/static/img/homepage/platforms.svg').default,
    description: (
      <>
        Support Linux and macOS, on x86 and ARM.
      </>
    ),
  },
  {
    title: 'Well-designed assembly code',
    Svg: require('@site/static/img/homepage/binary.svg').default,
    description: (
      <>
        Reduce overhead on the critical path.
      </>
    ),
  },
  {
    title: 'Fully compatible API toward C++ std and POSIX',
    Svg: require('@site/static/img/homepage/api.svg').default,
    description: (
      <>
        Easy to learn. Less effort to integrate to a legacy codebase.        
      </>
    ),
  },
];

function Feature({Svg, title, description}) {
  return (
    <div className={clsx('col col--4')}>
      <div className="text--center">
        <Svg className={styles.featureSvg} role="img" />
      </div>
      <div className="text--center padding-horiz--md">
        <h3>{title}</h3>
        <p>{description}</p>
      </div>
    </div>
  );
}

export default function HomepageFeatures() {
  return (
    <section className={styles.features}>
      <div className="container">
        <div className="row">
          {FeatureList.map((props, idx) => (
            <Feature key={idx} {...props} />
          ))}
        </div>
      </div>
    </section>
  );
}
